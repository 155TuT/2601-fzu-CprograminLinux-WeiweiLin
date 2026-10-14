#!/usr/bin/env python3
"""One turn's durable streaming adapter for an already-running local model.

The C worker owns sessions, client sockets, cancellation and response.txt.
This process owns model templates, raw generation, thinking and model metrics.
Its stdout is exclusively binary T4 frames; diagnostics go to stderr.
"""

import argparse
import json
import math
from pathlib import Path
import re
import sys
import time

from journal import Journal, atomic_bytes, atomic_json
from stream import ThinkingParser
from transport import (DELTA, END, ERROR, THINK, MAX_RAW, MAX_RESPONSE,
                       Frames, LocalServer, checked_text, normalize_config)


def read_text(path, limit=MAX_RESPONSE):
    with Path(path).open("rb") as source:
        data = source.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"{path}: text exceeds {limit} bytes")
    return checked_text(data.decode("utf-8", "strict"), str(path))


def history_messages(turn):
    """Use completed structured turns; cancelled answers never become context."""
    match = re.fullmatch(r"turn-([0-9]{8})", turn.name)
    if not match:
        raise ValueError("turn directory must be named turn-00000001, etc.")
    current = int(match.group(1))
    messages = []
    total = 0
    for prior in sorted(turn.parent.iterdir()):
        identity = re.fullmatch(r"turn-([0-9]{8})", prior.name)
        if (identity is None or int(identity.group(1)) >= current
                or not prior.is_dir() or not (prior / "done").is_file()
                or (prior / "cancelled").exists()):
            continue
        question = read_text(prior / "prompt.txt", 4096)
        answer = read_text(prior / "response.txt")
        total += len(question.encode("utf-8")) + len(answer.encode("utf-8"))
        if total > MAX_RAW:
            raise ValueError("completed history exceeds 32 MiB; start a new session")
        messages.extend(({"role": "user", "content": question},
                         {"role": "assistant", "content": answer}))
    messages.append({"role": "user", "content": read_text(turn / "prompt.txt", 4096)})
    return messages


def model_identity(properties):
    model_path = checked_text(properties.get("model_path"), "llama-server model_path")
    template = checked_text(properties.get("chat_template"), "llama-server chat_template")
    identity = {"model_path": model_path, "chat_template": template,
                "build_info": properties.get("build_info", "unknown")}
    default = properties.get("default_generation_settings", {})
    capacity = default.get("n_ctx") if isinstance(default, dict) else None
    if type(capacity) is int and capacity > 0:
        identity["context_tokens"] = capacity
    # Same path with a replaced GGUF must not silently change a resumed turn.
    try:
        metadata = Path(model_path).stat()
    except (FileNotFoundError, OSError):
        pass
    else:
        identity["model_file"] = {"size": metadata.st_size, "mtime_ns": metadata.st_mtime_ns}
    return identity


def context_capacity_compatible(saved, current):
    """Accept unchanged identity/config, or only an explicit capacity increase.

    Existing turns still use their frozen context ceiling. A larger runtime
    must never silently change sampling, model, template or other identities.
    """
    if saved == current:
        return True
    if not isinstance(saved, dict) or not isinstance(current, dict):
        return False
    old_capacity, new_capacity = saved.get("context_tokens"), current.get("context_tokens")
    return (type(old_capacity) is int and type(new_capacity) is int
            and 0 < old_capacity <= new_capacity
            and {key: value for key, value in saved.items() if key != "context_tokens"}
            == {key: value for key, value in current.items() if key != "context_tokens"})


def snapshot_for(turn, config, server):
    path = turn / "model-snapshot.json"
    if path.exists():
        document = json.loads(read_text(path, MAX_RAW))
        if not isinstance(document, dict) or document.get("version") != 1:
            raise ValueError("unsupported model snapshot")
        if not context_capacity_compatible(document.get("config"), config):
            raise ValueError("model config changed during this turn; restore the saved config or start a new turn")
        checked_text(document.get("prompt"), "frozen prompt")
        if document.get("messages", [])[-1:] != [{"role": "user", "content": read_text(turn / "prompt.txt", 4096)}]:
            raise ValueError("prompt changed after the model template was frozen")
    else:
        identity = model_identity(server.json_request("/props"))
        messages = history_messages(turn)
        rendered = server.json_request("/apply-template", {
            "messages": messages, "model": config["model"],
            "add_generation_prompt": True, "reasoning_format": "none",
            "chat_template_kwargs": {"enable_thinking": config["thinking"]},
        })
        prompt = checked_text(rendered.get("prompt"), "rendered prompt")
        # User text can mention tags; only the rendered assistant prefill opens reasoning.
        initial_thinking = prompt.rstrip().endswith("<think>")
        ThinkingParser(config["thinking"], initial_thinking)
        document = {"version": 1, "config": config, "identity": identity,
                    "messages": messages, "prompt": prompt,
                    "initial_thinking": initial_thinking}
        atomic_json(path, document)
    # Derived readable evidence can be rebuilt if the process died after snapshot fsync.
    atomic_bytes(turn / "rendered-prompt.txt", document["prompt"].encode("utf-8"))
    return document


def sanitized_timings(event):
    raw = event.get("timings", {})
    if not isinstance(raw, dict):
        return {}
    return {key: value for key, value in raw.items()
            if type(value) in (int, float) and math.isfinite(value)}


REPLAY_COUNTS = ("generated_tokens", "prompt_tokens", "context_tokens",
                 "attempt_max_tokens", "raw_prefix_bytes")
REPLAY_DURATIONS = ("elapsed_seconds", "first_content_seconds", "tokens_per_second")


def completed_metrics(turn, finished, final_bytes):
    """Restore observed completion statistics without timing the disk replay.

    New journals contain the final attempt's statistics before END is emitted.
    Older journals can use a completed derived metrics file or native timings;
    a missing count or duration remains absent, rather than becoming zero.
    """
    result = {}
    try:
        saved = json.loads(read_text(turn / "metrics.json", 65536))
    except (OSError, ValueError):
        saved = {}
    if not isinstance(saved, dict) or saved.get("phase") != "completed" or saved.get("final_bytes") != final_bytes:
        saved = {}
    durable = finished.get("metrics", {})
    for source in (saved, durable if isinstance(durable, dict) else {}):
        for key in REPLAY_COUNTS:
            value = source.get(key)
            if type(value) is int and value >= 0:
                result[key] = value
        for key in REPLAY_DURATIONS:
            value = source.get(key)
            if type(value) in (int, float) and math.isfinite(value) and value >= 0:
                result[key] = value
            elif key == "first_content_seconds" and key in source and value is None:
                result[key] = None
    timings = sanitized_timings(finished)
    count = timings.get("predicted_n")
    if type(count) is int and count >= 0:
        result["generated_tokens"] = count
    speed = timings.get("predicted_per_second")
    if type(speed) in (int, float) and speed >= 0:
        result["tokens_per_second"] = speed
    return result


class TurnAdapter:
    def __init__(self, turn, context, offset, config, frames, runtime_config=None):
        self.turn, self.context, self.offset = turn, context, offset
        self.config, self.frames = config, frames
        self.runtime_config = runtime_config
        self.final_offset = 0
        self.thinking_offset = 0
        self.metrics = {"phase": "loading", "model": config["model"],
                        "endpoint": config["endpoint"], "thinking": config["thinking"],
                        "resumed_at": offset, "generated_tokens": 0}

    def metrics_save(self, phase, **fields):
        self.metrics.update(phase=phase, final_bytes=self.final_offset,
                            thinking_bytes=self.thinking_offset, **fields)
        atomic_json(self.turn / "metrics.json", self.metrics)

    def emit(self, pieces, thinking_file):
        for kind, content in pieces:
            size = len(content.encode("utf-8"))
            if kind == "thinking":
                if self.thinking_offset + size > MAX_RESPONSE:
                    raise ValueError("thinking exceeds 16 MiB")
                thinking_file.write(content.encode("utf-8"))
                thinking_file.flush()
                # This is derived evidence; the raw journal was fsynced first.
                self.thinking_offset = self.frames.chunks(THINK, self.thinking_offset, content)
            else:
                if self.final_offset + size > MAX_RESPONSE:
                    raise ValueError("final answer exceeds 16 MiB")
                for character in content:
                    self.frames.send(DELTA, self.final_offset, character)
                    self.final_offset += len(character.encode("utf-8"))

    def execute(self):
        transport_config = self.config
        if self.runtime_config is not None:
            # This separate transport copy never enters model-snapshot.json;
            # the full frozen config still has its original strict comparison.
            transport_config = {**self.config, "endpoint": self.runtime_config["endpoint"]}
        server = LocalServer(transport_config)
        self.frames.status(phase="loading", model=self.config["model"], resumed_at=self.offset)
        frozen = snapshot_for(self.turn, self.config, server)
        journal = Journal(self.turn / "raw-generation.jsonl")
        thinking_file = None
        stream = None
        try:
            parser = ThinkingParser(self.config["thinking"], frozen["initial_thinking"])
            thoughts, answers, raw = [], [], []
            for record in journal.records:
                raw.append(record["text"])
                for kind, content in parser.feed(record["text"]):
                    (thoughts if kind == "thinking" else answers).append(content)
            if journal.finished:
                answers.extend(content for kind, content in parser.finish() if kind == "final")
            answer = "".join(answers).encode("utf-8")
            thought = "".join(thoughts)
            if len(answer) > MAX_RESPONSE or len(thought.encode("utf-8")) > MAX_RESPONSE:
                raise ValueError("derived model text exceeds 16 MiB")
            response = read_text(self.turn / "response.txt").encode("utf-8")
            if len(response) != self.offset or response != answer[:self.offset]:
                raise ValueError("response.txt does not match the durable raw-generation prefix")
            if self.offset > len(answer):
                raise ValueError("resume offset exceeds the durable answer")
            suffix = answer[self.offset:].decode("utf-8", "strict")
            atomic_bytes(self.turn / "thinking.txt", thought.encode("utf-8"))
            self.frames.status(phase="replaying", thinking_reset=True,
                               durable_answer_bytes=len(answer), durable_raw_bytes=journal.raw_bytes)
            self.thinking_offset = self.frames.chunks(THINK, 0, thought)
            self.final_offset = self.offset
            for character in suffix:
                self.frames.send(DELTA, self.final_offset, character)
                self.final_offset += len(character.encode("utf-8"))
            if journal.finished:
                self.metrics.pop("generated_tokens", None)
                self.metrics.update(completed_metrics(self.turn, journal.finished, len(answer)))
                self.metrics_save("completed", stop_type=journal.finished["stop_type"], replay_only=True,
                                  timings=sanitized_timings(journal.finished))
                self.frames.status(**self.metrics)
                self.frames.send(END, self.final_offset)
                return
            # Never render another template on resume, and never resample old bytes.
            # Fresh inference begins strictly after the persisted raw continuation.
            identity = model_identity(server.json_request("/props"))
            if not context_capacity_compatible(frozen["identity"], identity):
                raise ValueError("local model, build, template or context changed; restore the frozen runtime before resuming")
            self.metrics["endpoint"] = getattr(server, "endpoint", self.config["endpoint"])
            prompt = frozen["prompt"] + "".join(raw)
            tokens = server.json_request("/tokenize", {"content": prompt, "add_special": True,
                                                       "parse_special": True}).get("tokens")
            if not isinstance(tokens, list) or any(type(token) is not int for token in tokens):
                raise ValueError("/tokenize returned an invalid token list")
            # A resource upgrade permits old snapshots to reconnect, but the
            # old turn's budget stays frozen; only new turns use the larger cap.
            frozen_capacity = frozen["config"]["context_tokens"]
            capacity = min(frozen_capacity, identity.get("context_tokens", frozen_capacity))
            remaining = capacity - len(tokens) - 8
            if remaining <= 0:
                raise ValueError(f"context exhausted: {len(tokens)} prompt tokens, capacity {capacity}; /stop this turn and start a new session")
            budget = min(self.config["max_tokens"], remaining)
            payload = {"prompt": prompt, "stream": True, "n_predict": budget,
                       "cache_prompt": True, "return_progress": True,
                       "temperature": self.config["temperature"], "top_p": self.config["top_p"],
                       "top_k": self.config["top_k"], "min_p": self.config["min_p"],
                       "presence_penalty": self.config["presence_penalty"],
                       "model": self.config["model"]}
            self.metrics_save("processing", prompt_tokens=len(tokens), context_tokens=capacity,
                              attempt_max_tokens=budget, raw_prefix_bytes=journal.raw_bytes)
            self.frames.status(**self.metrics)
            thinking_file = (self.turn / "thinking.txt").open("ab")
            start = time.monotonic()
            first_content = None
            last_status = start
            generated_tokens = 0
            stream = server.completion(payload)
            for event in stream:
                if event is None:
                    break
                content = checked_text(event.get("content", ""), "model content")
                token_ids = event.get("tokens", [])
                # b11429 prompt-progress SSE includes a placeholder tokens=[0].
                # Empty generated text can still contain real sampled token IDs.
                token_count = (len(token_ids) if isinstance(token_ids, list)
                               and "prompt_progress" not in event else 0)
                if content:
                    if first_content is None:
                        first_content = time.monotonic()
                    journal.append({"kind": "raw", "text": content, "token_count": token_count})
                    self.emit(parser.feed(content), thinking_file)
                native_count = event.get("tokens_predicted")
                if type(native_count) is int and native_count >= 0:
                    generated_tokens = native_count
                else:
                    generated_tokens += token_count
                self.metrics["generated_tokens"] = generated_tokens
                now = time.monotonic()
                if now - last_status >= 1:
                    self.metrics_save("generating", elapsed_seconds=now - start,
                                      tokens_per_second=generated_tokens / max(now - start, 0.001))
                    self.frames.status(**self.metrics)
                    last_status = now
                if type(event.get("stop", False)) is not bool:
                    raise ValueError("llama-server stop flag is not boolean")
                if not event.get("stop", False):
                    continue
                stop_type = event.get("stop_type")
                timings = sanitized_timings(event)
                native_total = timings.get("predicted_n")
                if type(native_total) is int and native_total >= 0:
                    generated_tokens = native_total
                    self.metrics["generated_tokens"] = generated_tokens
                record = {"kind": "stopped", "stop_type": str(stop_type), "timings": timings}
                if event.get("truncated", False) or stop_type not in ("eos", "word"):
                    journal.append(record)
                    self.metrics_save("interrupted", stop_type=stop_type, timings=timings)
                    raise RuntimeError(f"generation stopped at {stop_type} (context_truncated={bool(event.get('truncated', False))}); raw checkpoint retained; rerun the same turn to continue, or /stop to cancel")
                try:
                    tail = parser.finish()
                except ValueError:
                    journal.append(record)
                    raise
                record["kind"] = "finished"
                elapsed = time.monotonic() - start
                completion_metrics = {
                    "elapsed_seconds": elapsed,
                    "first_content_seconds": None if first_content is None else first_content - start,
                    "generated_tokens": generated_tokens,
                    "tokens_per_second": timings.get("predicted_per_second", generated_tokens / max(elapsed, 0.001)),
                }
                # The journal is authoritative even if the adapter exits before
                # the derived metrics.json file can be replaced.
                record["metrics"] = {key: self.metrics[key] for key in REPLAY_COUNTS if key in self.metrics}
                record["metrics"].update(completion_metrics)
                journal.append(record)
                self.emit(tail, thinking_file)
                self.metrics_save("completed", stop_type=stop_type, timings=timings,
                                  **completion_metrics)
                self.frames.status(**self.metrics)
                self.frames.send(END, self.final_offset)
                return
            raise RuntimeError("model stream ended without an EOS/stop record; raw checkpoint retained for continuation")
        except Exception as error:
            self.metrics_save("error", error=str(error))
            raise
        finally:
            if stream is not None:
                stream.close()
            if thinking_file is not None:
                thinking_file.close()
            journal.close()


def main():
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument("--turn", required=True, type=Path)
    argument_parser.add_argument("--context", required=True, type=Path,
                                 help="reserved C interface; structured completed turns are authoritative")
    argument_parser.add_argument("--offset", required=True, type=int)
    argument_parser.add_argument("--config", required=True, type=Path)
    argument_parser.add_argument("--runtime-config", type=Path,
                                 help="current config supplies only transport endpoint; turn inference settings stay frozen")
    args = argument_parser.parse_args()
    frames = Frames()
    try:
        if not 0 <= args.offset <= MAX_RESPONSE:
            raise ValueError("offset must be 0..16777216 UTF-8 bytes")
        config = normalize_config(json.loads(read_text(args.config, 65536)))
        runtime_config = (normalize_config(json.loads(read_text(args.runtime_config, 65536)))
                          if args.runtime_config is not None else None)
        TurnAdapter(args.turn, args.context, args.offset, config, frames, runtime_config).execute()
        return 0
    except BrokenPipeError:
        print("model adapter: C worker disconnected; raw checkpoint retained", file=sys.stderr)
        return 1
    except Exception as error:
        message = str(error)
        print("model adapter: " + message, file=sys.stderr)
        try:
            frames.send(ERROR, args.offset, message.encode("utf-8", "replace")[:4096].decode("utf-8", "ignore"))
        except (OSError, ValueError):
            pass
        return 1


if __name__ == "__main__":
    sys.exit(main())
