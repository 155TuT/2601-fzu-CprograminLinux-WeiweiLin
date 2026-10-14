"""Durable, append-only raw-generation authority and atomic small snapshots."""

import json
import os
from pathlib import Path
import tempfile

from transport import MAX_RAW, checked_text


def atomic_bytes(path, content):
    path = Path(path)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".tmp-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def atomic_json(path, document):
    atomic_bytes(path, (json.dumps(document, ensure_ascii=False, sort_keys=True,
                                 indent=2, allow_nan=False) + "\n").encode("utf-8"))


class Journal:
    """An incomplete final JSONL line is uncommitted and safely discarded.

    Newline-terminated malformed records are corruption and fail closed. Each
    chunk is fsynced before its corresponding display frames can be emitted.
    A 'finished' record is authoritative even if C did not create turn/done.
    """

    def __init__(self, path):
        self.path = Path(path)
        self.raw_bytes = 0
        self.finished = None
        self.last_stop = None
        self.records = self._load()
        self.output = self.path.open("ab", buffering=0)
        directory = os.open(self.path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)

    def _load(self):
        records = []
        if not self.path.exists():
            return records
        with self.path.open("r+b", buffering=0) as source:
            good = 0
            while True:
                line = source.readline(2 * 1024 * 1024 + 1)
                if not line:
                    break
                if len(line) > 2 * 1024 * 1024:
                    raise ValueError("raw journal line exceeds limit")
                if not line.endswith(b"\n"):
                    source.truncate(good)
                    os.fsync(source.fileno())
                    break
                record = json.loads(line.decode("utf-8"))
                if not isinstance(record, dict) or self.finished is not None:
                    raise ValueError("invalid raw journal record ordering")
                kind = record.get("kind")
                if kind == "raw":
                    text = checked_text(record.get("text"), "journal text")
                    self.raw_bytes += len(text.encode("utf-8"))
                    if self.raw_bytes > MAX_RAW:
                        raise ValueError("raw generation exceeds journal byte limit")
                    records.append(record)
                elif kind in ("stopped", "finished"):
                    if not isinstance(record.get("stop_type"), str):
                        raise ValueError("invalid raw journal stop record")
                    self.last_stop = record
                    if kind == "finished":
                        if record["stop_type"] not in ("eos", "word"):
                            raise ValueError("a token limit cannot be a finished journal")
                        self.finished = record
                else:
                    raise ValueError("unknown raw journal record")
                good = source.tell()
        return records

    def append(self, record):
        if self.finished is not None:
            raise ValueError("cannot append after finished generation")
        kind = record.get("kind")
        if kind == "raw":
            text = checked_text(record.get("text"), "model content")
            size = len(text.encode("utf-8"))
            if self.raw_bytes + size > MAX_RAW:
                raise ValueError("raw generation exceeds journal byte limit")
        payload = (json.dumps(record, ensure_ascii=False, separators=(",", ":"),
                              allow_nan=False) + "\n").encode("utf-8")
        if len(payload) > 2 * 1024 * 1024:
            raise ValueError("raw journal record exceeds byte limit")
        pending = memoryview(payload)
        while pending:
            count = self.output.write(pending)
            if not count:
                raise OSError("short raw journal write")
            pending = pending[count:]
        os.fsync(self.output.fileno())
        if kind == "raw":
            self.raw_bytes += size
            self.records.append(record)
        elif kind in ("stopped", "finished"):
            self.last_stop = record
            if kind == "finished":
                self.finished = record

    def close(self):
        self.output.close()
