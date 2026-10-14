"""Local-only llama.cpp HTTP and the generator's binary frame transport.

API reference: llama.cpp d81235049384534c167caea52b85a694f6103d14,
tools/server/README.md: /props, /apply-template, /tokenize and /completion.
http.client deliberately bypasses environment proxy settings and redirects.
"""

import http.client
import ipaddress
import json
import math
import os
import struct
from urllib.parse import urlsplit

MAGIC = 0x54345331
DELTA, END, ERROR, THINK, STATUS = 6, 7, 8, 10, 13
MAX_FRAME = 4096
MAX_RESPONSE = 16 * 1024 * 1024
MAX_RAW = 32 * 1024 * 1024


def checked_text(value, label):
    if not isinstance(value, str) or "\0" in value:
        raise ValueError(f"{label} must be NUL-free text")
    value.encode("utf-8", "strict")
    return value


def normalize_config(document):
    if not isinstance(document, dict):
        raise ValueError("model config must be a JSON object")
    defaults = {
        "endpoint": "http://127.0.0.1:8081", "model": "local-model",
        "thinking": True, "max_tokens": 4096, "context_tokens": 4096,
        "temperature": 0.6, "top_p": 0.95, "top_k": 20,
        "min_p": 0.0, "presence_penalty": 0.0,
        "model_file": "", "slots": 1,
    }
    unknown = set(document) - set(defaults)
    if unknown:
        raise ValueError("unknown model config fields: " + ", ".join(sorted(unknown)))
    config = {**defaults, **document}
    checked_text(config["model"], "model")
    checked_text(config["model_file"], "model_file")
    if not config["model"]:
        raise ValueError("model identity cannot be empty")
    checked_text(config["endpoint"], "endpoint")
    if config["endpoint"] == "auto":
        if not config["model_file"]:
            raise ValueError("automatic model discovery requires model_file")
    else:
        parsed = urlsplit(config["endpoint"])
        if (parsed.scheme != "http" or parsed.username is not None
                or parsed.password is not None or parsed.path not in ("", "/")
                or parsed.query or parsed.fragment or parsed.hostname is None):
            raise ValueError("endpoint must be auto or an http:// loopback IP and port, with no path/auth/query")
        try:
            address = ipaddress.ip_address(parsed.hostname)
            port = 80 if parsed.port is None else parsed.port
        except ValueError as error:
            raise ValueError("endpoint must use a literal loopback IP, never a hostname") from error
        if not address.is_loopback or "%" in parsed.hostname or not 1 <= port <= 65535:
            raise ValueError("only a literal loopback endpoint is permitted")
        host = f"[{address}]" if address.version == 6 else str(address)
        config["endpoint"] = f"http://{host}:{port}"
    if type(config["thinking"]) is not bool:
        raise ValueError("thinking must be true or false")
    for name, minimum, maximum in (("max_tokens", 1, 131072),
                                   ("context_tokens", 256, 1048576),
                                   ("top_k", 0, 10000), ("slots", 1, 64)):
        value = config[name]
        if type(value) is not int or not minimum <= value <= maximum:
            raise ValueError(f"{name} must be an integer in {minimum}..{maximum}")
    for name, minimum, maximum in (("temperature", 0.0, 2.0),
                                   ("top_p", 0.0, 1.0),
                                   ("min_p", 0.0, 1.0),
                                   ("presence_penalty", -2.0, 2.0)):
        value = config[name]
        if type(value) not in (int, float) or not math.isfinite(value) or not minimum <= value <= maximum:
            raise ValueError(f"{name} must be a finite number in {minimum}..{maximum}")
    return config


class LocalServer:
    """Each request owns its connection; closing a stream cancels its inference."""

    def __init__(self, config, timeout=600):
        self.config = config
        self.endpoint = config["endpoint"]
        parsed = urlsplit(self.endpoint) if self.endpoint != "auto" else None
        self.host, self.port = (parsed.hostname, parsed.port) if parsed else (None, None)
        self.timeout = timeout

    def _request(self, path, document=None):
        if self.host is None:
            from discovery import resolve_endpoint
            self.endpoint = resolve_endpoint(self.config)
            parsed = urlsplit(self.endpoint)
            self.host, self.port = parsed.hostname, parsed.port
        connection = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        body = None if document is None else json.dumps(document, ensure_ascii=False, allow_nan=False).encode("utf-8")
        try:
            connection.request("GET" if document is None else "POST", path, body,
                               {"Content-Type": "application/json", "Connection": "close"})
            response = connection.getresponse()
            if response.status != 200:
                message = response.read(MAX_FRAME).decode("utf-8", "replace")
                raise RuntimeError(f"local llama-server {path}: HTTP {response.status}: {message}")
            return connection, response
        except BaseException:
            connection.close()
            raise

    def json_request(self, path, document=None):
        connection, response = self._request(path, document)
        try:
            body = response.read(MAX_RAW + 1)
            if len(body) > MAX_RAW:
                raise ValueError(f"{path} JSON response exceeds {MAX_RAW} bytes")
            result = json.loads(body.decode("utf-8"))
            if not isinstance(result, dict):
                raise ValueError(f"{path} must return a JSON object")
            return result
        finally:
            response.close()
            connection.close()

    def completion(self, document):
        connection, response = self._request("/completion", document)
        data = []
        event_bytes = 0
        try:
            while True:
                line = response.readline(1024 * 1024 + 1)
                if len(line) > 1024 * 1024:
                    raise ValueError("oversized SSE line")
                if not line:
                    if data:
                        yield self._event(data)
                    break
                line = line.rstrip(b"\r\n")
                if not line:
                    if data:
                        event = self._event(data)
                        data, event_bytes = [], 0
                        if event is None:
                            break
                        yield event
                elif line.startswith(b"data:"):
                    piece = line[5:]
                    if piece.startswith(b" "):
                        piece = piece[1:]
                    data.append(piece)
                    event_bytes += len(piece)
                    if event_bytes > 1024 * 1024:
                        raise ValueError("oversized SSE event")
                # SSE comments/pings and optional event/id fields carry no text.
        finally:
            response.close()
            connection.close()

    @staticmethod
    def _event(lines):
        data = b"\n".join(lines)
        if data == b"[DONE]":
            return None
        event = json.loads(data.decode("utf-8"))
        if not isinstance(event, dict):
            raise ValueError("SSE data must be a JSON object")
        if "error" in event:
            raise RuntimeError("local inference error: " + json.dumps(event["error"], ensure_ascii=False))
        return event


class Frames:
    def __init__(self, fd=1):
        self.fd = fd

    def send(self, kind, offset=0, text=""):
        payload = checked_text(text, "frame").encode("utf-8")
        if len(payload) > MAX_FRAME or not 0 <= offset <= 0xffffffff:
            raise ValueError("frame length or offset out of range")
        packet = struct.pack("!IIIII", MAGIC, kind, 0, offset, len(payload)) + payload
        remaining = memoryview(packet)
        while remaining:
            count = os.write(self.fd, remaining)
            if not count:
                raise BrokenPipeError("generator frame socket closed")
            remaining = remaining[count:]

    def chunks(self, kind, offset, text):
        pending, size = [], 0
        for character in checked_text(text, "stream"):
            width = len(character.encode("utf-8"))
            if size + width > MAX_FRAME:
                self.send(kind, offset, "".join(pending))
                offset += size
                pending, size = [], 0
            pending.append(character)
            size += width
        if pending:
            self.send(kind, offset, "".join(pending))
            offset += size
        return offset

    def status(self, **fields):
        self.send(STATUS, 0, json.dumps(fields, ensure_ascii=False, separators=(",", ":"), allow_nan=False))
