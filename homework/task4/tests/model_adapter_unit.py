#!/usr/bin/env python3
"""Offline checks for raw-prefix recovery, tag splitting and local-only config."""

import json
import itertools
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src" / "model"))
from adapter import TurnAdapter, history_messages  # noqa: E402
from journal import Journal  # noqa: E402
from stream import ThinkingParser  # noqa: E402
from transport import DELTA, END, THINK, MAX_FRAME, Frames, normalize_config  # noqa: E402


class CapturedFrames:
    def __init__(self):
        self.records = []

    def send(self, kind, offset=0, text=""):
        self.records.append((kind, offset, text))

    def chunks(self, kind, offset, text):
        if text:
            self.send(kind, offset, text)
        return offset + len(text.encode("utf-8"))

    def status(self, **fields):
        self.records.append((13, 0, fields))


class FakeServer:
    calls = []
    completions = []
    template = "assistant\n<think>\n"
    capacity = 4096
    token_count = 3
    identity_overrides = {}

    def __init__(self, config):
        self.config = config

    def json_request(self, path, document=None):
        self.calls.append((path, document))
        if path == "/props":
            return {"model_path": "/nonexistent/unit.gguf", "chat_template": "unit-jinja",
                    "build_info": "unit", "default_generation_settings": {"n_ctx": self.capacity},
                    **self.identity_overrides}
        if path == "/apply-template":
            return {"prompt": self.template}
        if path == "/tokenize":
            return {"tokens": list(range(self.token_count))}
        raise AssertionError(path)

    def completion(self, document):
        self.calls.append(("/completion", document))
        for event in self.completions:
            yield event


class AdapterChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="task4-model-unit-")
        self.root = Path(self.temporary.name)
        self.turn = self.root / "turn-00000001"
        self.turn.mkdir()
        (self.turn / "prompt.txt").write_text("提问", encoding="utf-8")
        (self.turn / "response.txt").write_bytes(b"")
        self.config = normalize_config({"model": "unit-model"})
        FakeServer.calls = []
        FakeServer.template = "assistant\n<think>\n"
        FakeServer.completions = []
        FakeServer.capacity = 4096
        FakeServer.token_count = 3
        FakeServer.identity_overrides = {}

    def tearDown(self):
        self.temporary.cleanup()

    def test_marker_every_split(self):
        raw = "<think>思考内容</think>\n答案中提到<think>也保留"
        for width in range(1, len(raw) + 1):
            parser = ThinkingParser()
            output = []
            for start in range(0, len(raw), width):
                output += parser.feed(raw[start:start + width])
            output += parser.finish()
            self.assertEqual("".join(text for kind, text in output if kind == "thinking"), "思考内容")
            self.assertEqual("".join(text for kind, text in output if kind == "final"), "\n答案中提到<think>也保留")

    def test_template_prefill_and_unfinished_thinking(self):
        parser = ThinkingParser(initial_thinking=True)
        self.assertEqual(parser.feed("已有思考</thi"), [("thinking", "已有思考")])
        self.assertEqual(parser.feed("nk>答案"), [("final", "答案")])
        parser = ThinkingParser(initial_thinking=True)
        parser.feed("尚未闭合")
        with self.assertRaises(ValueError):
            parser.finish()
        with self.assertRaises(ValueError):
            ThinkingParser(thinking=False, initial_thinking=True)

    def test_journal_all_torn_positions(self):
        path = self.turn / "raw-generation.jsonl"
        journal = Journal(path)
        journal.append({"kind": "raw", "text": "中"})
        prefix = path.read_bytes()
        journal.append({"kind": "raw", "text": "</think>答案"})
        complete = path.read_bytes()
        journal.close()
        for cut in range(len(prefix), len(complete)):
            path.write_bytes(complete[:cut])
            loaded = Journal(path)
            self.assertEqual([row["text"] for row in loaded.records], ["中"])
            loaded.close()
            self.assertEqual(path.read_bytes(), prefix)
        path.write_bytes(prefix + b"not-json\n")
        with self.assertRaises(ValueError):
            Journal(path)

    def test_resume_uses_frozen_raw_prefix(self):
        frames = CapturedFrames()
        FakeServer.completions = [{"content": "思考</thi", "tokens": [1]},
                                  {"stop": True, "stop_type": "limit"}]
        with patch("adapter.LocalServer", FakeServer):
            with self.assertRaises(RuntimeError):
                TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, frames).execute()
            FakeServer.calls = []
            FakeServer.completions = [{"content": "nk>答", "tokens": [2]},
                                      {"content": "案", "tokens": [3]},
                                      {"stop": True, "stop_type": "eos", "timings": {"predicted_n": 2}}]
            frames = CapturedFrames()
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, frames).execute()
        calls = dict(FakeServer.calls)
        self.assertNotIn("/apply-template", calls)
        self.assertEqual(calls["/completion"]["prompt"], FakeServer.template + "思考</thi")
        self.assertEqual("".join(row[2] for row in frames.records if row[0] == DELTA), "答案")
        self.assertEqual([row for row in frames.records if row[0] == END], [(END, 6, "")])
        self.assertEqual((self.turn / "thinking.txt").read_text(), "思考")

    def test_raw_ahead_of_c_response_replays_without_http(self):
        frames = CapturedFrames()
        FakeServer.completions = [{"content": "思考</think>甲乙", "tokens": [1]},
                                  {"stop": True, "stop_type": "eos"}]
        with patch("adapter.LocalServer", FakeServer):
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, frames).execute()
            (self.turn / "response.txt").write_text("甲", encoding="utf-8")
            FakeServer.calls = []
            frames = CapturedFrames()
            TurnAdapter(self.turn, self.root / "context.txt", 3, self.config, frames).execute()
        self.assertEqual(FakeServer.calls, [])
        self.assertEqual([row for row in frames.records if row[0] == DELTA], [(DELTA, 3, "乙")])
        self.assertEqual([row for row in frames.records if row[0] == END], [(END, 6, "")])

    def test_completed_replay_restores_durable_statistics_without_metrics_file(self):
        FakeServer.completions = [{"content": "思考</think>答案", "tokens": [1]},
                                  {"stop": True, "stop_type": "eos",
                                   "timings": {"predicted_n": 777, "predicted_per_second": 12.5}}]
        with patch("adapter.LocalServer", FakeServer):
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            path = self.turn / "metrics.json"
            original = json.loads(path.read_text())
            path.unlink()  # A finished journal must survive loss of derived metrics.
            FakeServer.calls = []
            frames = CapturedFrames()
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, frames).execute()
        restored = json.loads(path.read_text())
        self.assertEqual(FakeServer.calls, [])
        for key in ("generated_tokens", "elapsed_seconds", "first_content_seconds",
                    "tokens_per_second", "prompt_tokens", "context_tokens", "timings"):
            self.assertEqual(restored[key], original[key], key)
        self.assertTrue(restored["replay_only"])
        self.assertEqual(restored["generated_tokens"], 777)
        status = [row[2] for row in frames.records if row[0] == 13][-1]
        self.assertEqual(status["generated_tokens"], 777)

    def test_legacy_completed_replay_uses_available_statistics_only(self):
        FakeServer.completions = [{"content": "思考</think>答案", "tokens_predicted": 777},
                                  {"stop": True, "stop_type": "eos"}]
        with patch("adapter.LocalServer", FakeServer):
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            path = self.turn / "metrics.json"
            original = json.loads(path.read_text())
            journal = self.turn / "raw-generation.jsonl"
            records = [json.loads(line) for line in journal.read_text().splitlines()]
            records[-1].pop("metrics")
            journal.write_text("".join(json.dumps(record) + "\n" for record in records))
            FakeServer.calls = []
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            restored = json.loads(path.read_text())
            self.assertEqual(restored["generated_tokens"], 777)
            self.assertEqual(restored["elapsed_seconds"], original["elapsed_seconds"])
            path.unlink()
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            unknown = json.loads(path.read_text())
            self.assertNotIn("generated_tokens", unknown)
            self.assertNotIn("elapsed_seconds", unknown)
            # Older runtimes may have durable native counts without a sidecar.
            records[-1]["timings"] = {"predicted_n": 777}
            journal.write_text("".join(json.dumps(record) + "\n" for record in records))
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
        self.assertEqual(json.loads(path.read_text())["generated_tokens"], 777)
        self.assertEqual(FakeServer.calls, [])

    def test_prefix_mismatch_and_config_change_fail(self):
        FakeServer.completions = [{"content": "思考</think>答案"}, {"stop": True, "stop_type": "eos"}]
        with patch("adapter.LocalServer", FakeServer):
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            (self.turn / "response.txt").write_text("错", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "does not match"):
                TurnAdapter(self.turn, self.root / "context.txt", 3, self.config, CapturedFrames()).execute()
            changed = {**self.config, "temperature": 0.8}
            with self.assertRaisesRegex(ValueError, "config changed"):
                TurnAdapter(self.turn, self.root / "context.txt", 3, changed, CapturedFrames()).execute()

    def test_context_upgrade_preserves_old_turn_ceiling(self):
        FakeServer.completions = [{"content": "思考", "tokens": [1]},
                                  {"stop": True, "stop_type": "limit"}]
        with patch("adapter.LocalServer", FakeServer):
            with self.assertRaises(RuntimeError):
                TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            snapshot_before = (self.turn / "model-snapshot.json").read_bytes()
            FakeServer.capacity, FakeServer.token_count = 16384, 3800
            FakeServer.calls = []
            FakeServer.completions = [{"content": "</think>答案", "tokens": [2]},
                                      {"stop": True, "stop_type": "eos"}]
            enlarged = {**self.config, "context_tokens": 16384}
            TurnAdapter(self.turn, self.root / "context.txt", 0, enlarged, CapturedFrames()).execute()
        request = dict(FakeServer.calls)["/completion"]
        self.assertEqual(request["n_predict"], 4096 - 3800 - 8)
        self.assertEqual((self.turn / "model-snapshot.json").read_bytes(), snapshot_before)
        self.assertEqual(json.loads((self.turn / "metrics.json").read_text())["context_tokens"], 4096)

    def test_new_turn_uses_16k_ceiling(self):
        FakeServer.capacity, FakeServer.token_count = 16384, 4500
        FakeServer.completions = [{"content": "思考</think>答案", "tokens": [1]},
                                  {"stop": True, "stop_type": "eos"}]
        enlarged = {**self.config, "context_tokens": 16384}
        with patch("adapter.LocalServer", FakeServer):
            TurnAdapter(self.turn, self.root / "context.txt", 0, enlarged, CapturedFrames()).execute()
        self.assertEqual(dict(FakeServer.calls)["/completion"]["n_predict"], 4096)
        self.assertEqual(json.loads((self.turn / "metrics.json").read_text())["context_tokens"], 16384)

    def test_budget_upgrade_keeps_pending_turn_at_frozen_2048(self):
        """A deployment upgrade affects new turns, never an existing journal."""
        old_config = {**self.config, "context_tokens": 16384, "max_tokens": 2048}
        FakeServer.capacity = 16384
        FakeServer.completions = [{"content": "思考", "tokens": [1]},
                                  {"stop": True, "stop_type": "limit"}]
        with patch("adapter.LocalServer", FakeServer):
            with self.assertRaises(RuntimeError):
                TurnAdapter(self.turn, self.root / "context.txt", 0, old_config, CapturedFrames()).execute()
            snapshot_before = (self.turn / "model-snapshot.json").read_bytes()
            FakeServer.calls = []
            FakeServer.completions = [{"content": "</think>答案", "tokens": [2]},
                                      {"stop": True, "stop_type": "eos"}]
            runtime = {**old_config, "max_tokens": 4096}
            TurnAdapter(self.turn, self.root / "context.txt", 0, old_config,
                        CapturedFrames(), runtime).execute()
        self.assertEqual(dict(FakeServer.calls)["/completion"]["n_predict"], 2048)
        self.assertEqual((self.turn / "model-snapshot.json").read_bytes(), snapshot_before)

    def test_endpoint_move_keeps_snapshot_and_continuation_frozen(self):
        FakeServer.completions = [{"content": "思考</thi", "tokens": [1]},
                                  {"stop": True, "stop_type": "limit"}]
        with patch("adapter.LocalServer", FakeServer):
            with self.assertRaises(RuntimeError):
                TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            before = (self.turn / "model-snapshot.json").read_bytes()
            FakeServer.calls = []
            FakeServer.completions = [{"content": "nk>答案", "tokens": [2]},
                                      {"stop": True, "stop_type": "eos"}]
            moved = {**self.config, "endpoint": "http://127.0.0.1:54321"}
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config,
                        CapturedFrames(), moved).execute()
        self.assertEqual((self.turn / "model-snapshot.json").read_bytes(), before)
        self.assertNotIn("/apply-template", dict(FakeServer.calls))
        self.assertEqual(dict(FakeServer.calls)["/completion"]["prompt"], FakeServer.template + "思考</thi")

    def test_runtime_transport_override_retains_old_inference_config(self):
        FakeServer.completions = [{"content": "思考", "tokens": [1]},
                                  {"stop": True, "stop_type": "limit"}]
        connections = []
        class RecordServer(FakeServer):
            def __init__(self, config):
                connections.append(config)
                super().__init__(config)
        with patch("adapter.LocalServer", RecordServer):
            with self.assertRaises(RuntimeError):
                TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            before = (self.turn / "model-snapshot.json").read_bytes()
            FakeServer.capacity, FakeServer.token_count = 16384, 3800
            FakeServer.completions = [{"content": "</think>答案", "tokens": [2]},
                                      {"stop": True, "stop_type": "eos"}]
            runtime = {**self.config, "endpoint": "auto", "temperature": 0.9,
                       "context_tokens": 16384, "max_tokens": 4096}
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config,
                        CapturedFrames(), runtime).execute()
        request = dict(FakeServer.calls)["/completion"]
        self.assertEqual(connections[-1]["endpoint"], "auto")
        self.assertEqual(request["temperature"], self.config["temperature"])
        self.assertEqual(request["n_predict"], 4096 - 3800 - 8)
        self.assertEqual((self.turn / "model-snapshot.json").read_bytes(), before)

    def test_endpoint_move_still_rejects_changed_runtime_identity(self):
        FakeServer.completions = [{"content": "思考", "tokens": [1]},
                                  {"stop": True, "stop_type": "limit"}]
        with patch("adapter.LocalServer", FakeServer):
            with self.assertRaises(RuntimeError):
                TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            FakeServer.identity_overrides = {"build_info": "different-runtime-build"}
            runtime = {**self.config, "endpoint": "http://127.0.0.1:54321"}
            with self.assertRaisesRegex(ValueError, "model, build, template"):
                TurnAdapter(self.turn, self.root / "context.txt", 0, self.config,
                            CapturedFrames(), runtime).execute()

    def test_context_decrease_or_other_identity_change_is_rejected(self):
        FakeServer.completions = [{"content": "思考", "tokens": [1]},
                                  {"stop": True, "stop_type": "limit"}]
        with patch("adapter.LocalServer", FakeServer):
            with self.assertRaises(RuntimeError):
                TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            FakeServer.capacity = 2048
            with self.assertRaisesRegex(ValueError, "context changed"):
                TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            FakeServer.capacity = 16384
            for changed in ({"build_info": "other-build"}, {"chat_template": "other-template"},
                            {"model_path": "/nonexistent/other.gguf"}):
                FakeServer.identity_overrides = changed
                with self.assertRaisesRegex(ValueError, "model, build, template"):
                    TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
            FakeServer.identity_overrides = {}
            reduced = {**self.config, "context_tokens": 2048}
            with self.assertRaisesRegex(ValueError, "config changed"):
                TurnAdapter(self.turn, self.root / "context.txt", 0, reduced, CapturedFrames()).execute()

    def test_completed_history_excludes_cancelled_and_future(self):
        for number in (2, 3, 5):
            prior = self.root / f"turn-{number:08d}"
            prior.mkdir()
            (prior / "done").touch()
            (prior / "prompt.txt").write_text(f"问题{number}")
            (prior / "response.txt").write_text(f"答案{number}")
        (self.root / "turn-00000003" / "cancelled").touch()
        current = self.root / "turn-00000004"
        current.mkdir()
        (current / "prompt.txt").write_text("现在")
        self.assertEqual(history_messages(current), [
            {"role": "user", "content": "问题2"}, {"role": "assistant", "content": "答案2"},
            {"role": "user", "content": "现在"}])

    def test_endpoint_restrictions(self):
        for endpoint in ("https://127.0.0.1:8081", "http://localhost:8081", "http://8.8.8.8:8081",
                         "http://user@127.0.0.1:8081", "http://127.0.0.1:8081/v1", "http://127.0.0.1?x",
                         "http://127.0.0.1:0"):
            with self.assertRaises(ValueError):
                normalize_config({"endpoint": endpoint})
        self.assertEqual(normalize_config({"endpoint": "http://[::1]:8081"})["endpoint"], "http://[::1]:8081")

    def test_deployed_config_metadata(self):
        deployed = json.loads((Path(__file__).resolve().parents[1] / "config" / "model.json").read_text())
        # Deployment parameters may be tuned without changing this test;
        # normalization validates their supported types and ranges.
        config = normalize_config(deployed)
        self.assertEqual(config["model_file"], deployed["model_file"])
        self.assertTrue(config["model_file"])

    def test_frame_chunk_utf8_offsets_and_partial_writes(self):
        wire = bytearray()

        def partial_write(fd, content):
            count = min(7, len(content))
            wire.extend(content[:count])
            return count

        with patch("transport.os.write", partial_write):
            self.assertEqual(Frames().chunks(THINK, 0, "中" * 2000), 6000)
        position, expected_offset, pieces = 0, 0, []
        while position < len(wire):
            magic, kind, request, offset, length = struct.unpack("!IIIII", wire[position:position + 20])
            position += 20
            self.assertEqual((magic, kind, request, offset), (0x54345331, THINK, 0, expected_offset))
            self.assertLessEqual(length, MAX_FRAME)
            pieces.append(bytes(wire[position:position + length]).decode("utf-8"))
            position += length
            expected_offset += length
        self.assertEqual("".join(pieces), "中" * 2000)

    def test_prompt_progress_tokens_do_not_count_as_generation(self):
        progress = {"content": "", "tokens": [0], "stop": False,
                    "prompt_progress": {"total": 3, "cache": 0, "processed": 3, "time_ms": 1}}
        # Match pinned progress tokens=[0], then preserve a real empty-content token.
        FakeServer.completions = [progress, progress, progress,
                                  {"content": "思考", "tokens": [10]},
                                  {"content": "", "tokens": [11]},
                                  {"content": "</think>答案", "tokens": [12]},
                                  {"stop": True, "stop_type": "eos", "timings": {"predicted_n": 3}}]
        frames = CapturedFrames()
        with patch("adapter.LocalServer", FakeServer), patch("adapter.time.monotonic", side_effect=itertools.count(0, 1.1)):
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, frames).execute()
        live_counts = [row[2]["generated_tokens"] for row in frames.records
                       if row[0] == 13 and row[2].get("phase") == "generating"]
        self.assertEqual(live_counts, [0, 0, 0, 1, 2, 3, 3])
        metrics = json.loads((self.turn / "metrics.json").read_text())
        self.assertEqual(metrics["generated_tokens"], 3)
        self.assertEqual(metrics["timings"]["predicted_n"], 3)

    def test_limit_metrics_use_native_total_without_marking_done(self):
        FakeServer.completions = [
            {"content": "", "tokens": [0], "prompt_progress": {"processed": 3}},
            {"content": "未闭合的思考", "tokens": [10]},
            {"content": "", "tokens": [11]},
            {"stop": True, "stop_type": "limit", "timings": {"predicted_n": 7}},
        ]
        frames = CapturedFrames()
        with patch("adapter.LocalServer", FakeServer):
            with self.assertRaisesRegex(RuntimeError, "generation stopped at limit"):
                TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, frames).execute()
        metrics = json.loads((self.turn / "metrics.json").read_text())
        self.assertEqual(metrics["generated_tokens"], 7)
        self.assertEqual(metrics["timings"]["predicted_n"], 7)
        self.assertEqual(metrics["stop_type"], "limit")
        self.assertFalse(any(row[0] == END for row in frames.records))
        records = [json.loads(line) for line in (self.turn / "raw-generation.jsonl").read_text().splitlines()]
        self.assertEqual(records[-1]["kind"], "stopped")
        self.assertFalse(any(row["kind"] == "finished" for row in records))

    def test_cumulative_native_count_and_invalid_terminal_count(self):
        FakeServer.completions = [{"content": "思考</think>答案", "tokens": [10], "tokens_predicted": 4},
                                  {"stop": True, "stop_type": "eos", "tokens_predicted": 5,
                                   "timings": {"predicted_n": 1.5}}]
        with patch("adapter.LocalServer", FakeServer):
            TurnAdapter(self.turn, self.root / "context.txt", 0, self.config, CapturedFrames()).execute()
        metrics = json.loads((self.turn / "metrics.json").read_text())
        self.assertEqual(metrics["generated_tokens"], 5)


if __name__ == "__main__":
    unittest.main(verbosity=2)
