#!/usr/bin/env python3
"""只读检查 C 客户端的 T4CHECK2 journal；运行 C 程序不依赖此脚本。"""
import argparse
from pathlib import Path
import re
import struct
import sys


def checksum(header, body):
    value = 2166136261
    for byte in header[:12] + body:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def load(path):
    """返回 durable prefix；最后一条残缺记录只标记，不修改原文件。"""
    with Path(path).open("rb") as stream:
        if stream.readline() != b"T4CHECK2\n":
            raise ValueError("invalid checkpoint version")
        token = stream.readline().rstrip(b"\n")
        if not re.fullmatch(b"[0-9a-f]{32}", token):
            raise ValueError("invalid session token")
        numbers = stream.readline().split()
        if len(numbers) != 3 or any(not n.isdigit() for n in numbers):
            raise ValueError("invalid checkpoint header")
        request, plen, file_lines = map(int, numbers)
        if request > 10000 or plen > 4096:
            raise ValueError("checkpoint exceeds limits")
        prompt = stream.read(plen)
        if len(prompt) != plen or b"\0" in prompt:
            raise ValueError("invalid prompt")
        answer = bytearray()
        done = False
        torn = 0
        while header := stream.read(16):
            if len(header) != 16:
                torn = len(header)
                break
            kind, offset, length, digest = struct.unpack("!IIII", header)
            if length > 4 or offset != len(answer) or done:
                raise ValueError("invalid journal sequence")
            body = stream.read(length)
            if len(body) != length:
                torn = 16 + len(body)
                break
            if digest != checksum(header, body):
                raise ValueError("journal checksum mismatch")
            if kind == 6 and length:
                text = body.decode("utf-8", errors="strict")
                if len(text) != 1 or text == "\0" or len(answer) + length > 16777216:
                    raise ValueError("invalid character")
                answer.extend(body)
            elif kind == 7 and not length:
                done = True
            else:
                raise ValueError("invalid journal event")
    return dict(token=token.decode("ascii"), request=request, offset=len(answer),
                prompt=prompt, answer=bytes(answer), done=done,
                file_lines=file_lines, torn_bytes=torn)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("--answer", action="store_true", help="仅输出完整持久回复前缀")
    args = parser.parse_args()
    try:
        state = load(args.checkpoint)
    except (OSError, ValueError) as error:
        parser.exit(1, f"checkpoint: {error}\n")
    if args.answer:
        sys.stdout.buffer.write(state["answer"])
    else:
        print(f'session={state["token"]}')
        print(f'request={state["request"]} offset={state["offset"]} done={int(state["done"])} '
              f'file_lines={state["file_lines"]} torn_bytes={state["torn_bytes"]}')
        print(f'prompt={state["prompt"].decode("utf-8", errors="backslashreplace")}')


if __name__ == "__main__":
    main()
