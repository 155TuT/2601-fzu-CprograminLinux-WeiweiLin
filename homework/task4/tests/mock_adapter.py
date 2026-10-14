#!/usr/bin/env python3
"""Deterministic framed model adapter for transport and terminal tests only.

The C server freezes this fixture's configuration with each turn. It therefore
exercises the same typed adapter channel and restart offsets as the real model,
without starting an HTTP service or loading weights. Artificial pacing belongs
only to the fixture and is never a server option.
"""
import argparse
import base64
import json
from pathlib import Path
import struct
import sys
import time


def configuration(home, response, rate=0):
    path = Path(home) / f"fixture-{time.monotonic_ns()}.json"
    path.write_text(json.dumps({"response_base64": base64.b64encode(response).decode(),
                               "characters_per_second": rate}))
    return path


def send(kind, offset, payload=b""):
    sys.stdout.buffer.write(struct.pack("!IIIII", 0x54345331, kind, 0, offset, len(payload)) + payload)
    sys.stdout.buffer.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--turn")
    parser.add_argument("--context")
    parser.add_argument("--offset", type=int, required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--runtime-config")
    args = parser.parse_args()
    config = json.loads(Path(args.config).read_text())
    response = base64.b64decode(config["response_base64"])
    offset = args.offset
    text = response[offset:].decode("utf-8")
    rate = config["characters_per_second"]
    for character in text:
        payload = character.encode("utf-8")
        send(6, offset, payload)
        offset += len(payload)
        if rate:
            time.sleep(1 / rate)
    send(7, offset)


if __name__ == "__main__":
    main()
