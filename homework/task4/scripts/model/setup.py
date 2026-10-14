#!/usr/bin/env python3
"""Download pinned local llama.cpp/CUDA and original Qwen3.5-4B GGUF artifacts.

Only writes task4/.runtime. Downloads support restart by byte range; each curl
attempt has a deadline and a later attempt recomputes the resume position.
No system package, driver, remote inference API, or privileged installer is used.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import os

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / ".runtime"
LLAMA_TAG = "b11429"
LLAMA_COMMIT = "d81235049384534c167caea52b85a694f6103d14"
GGUF_REVISION = "e87f176479d0855a907a41277aca2f8ee7a09523"
SOURCE_REVISION = "851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a"
ASSETS = {
    "llama-b11429-bin-ubuntu-cuda-13.4-x64.tar.gz": (
        "https://github.com/ggml-org/llama.cpp/releases/download/b11429/"
        "llama-b11429-bin-ubuntu-cuda-13.4-x64.tar.gz",
        152519318,
        "8082b7eaa74a714c9fecca19128f751c8e32da763ee8096b8ad1e824da7621d3",
    ),
    "cudart-llama-b11429-bin-ubuntu-cuda-13.4-x64.tar.gz": (
        "https://github.com/ggml-org/llama.cpp/releases/download/b11429/"
        "cudart-llama-b11429-bin-ubuntu-cuda-13.4-x64.tar.gz",
        440236630,
        "93d18648d815b2bd624d83d82f653e1db97afb478f02064305fe3cf570040a6d",
    ),
    "Qwen3.5-4B-Q4_K_M.gguf": (
        "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/"
        + GGUF_REVISION + "/Qwen3.5-4B-Q4_K_M.gguf?download=true",
        2740937888,
        "00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4",
    ),
    "Qwen3.5-4B-Q5_K_M.gguf": (
        "https://huggingface.co/unsloth/Qwen3.5-4B-GGUF/resolve/"
        + GGUF_REVISION + "/Qwen3.5-4B-Q5_K_M.gguf?download=true",
        3143656608,
        "8814232b85594dcd46c50e5b8b29324a7efe9e746edbe8a3d1df3d3fce7aad39",
    ),
}


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def acquire(name: str, attempts: int) -> Path:
    url, size, digest = ASSETS[name]
    directory = RUNTIME / ("models" if name.endswith(".gguf") else "downloads")
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / name
    for attempt in range(attempts + 1):
        received = path.stat().st_size if path.exists() else 0
        if received > size:
            raise ValueError(f"{name}: existing file exceeds pinned size")
        if received == size:
            actual = sha256(path)
            if actual != digest:
                raise ValueError(f"{name}: SHA256 mismatch; expected {digest}, got {actual}")
            print(f"VERIFIED {name}: {size} bytes sha256={actual}", flush=True)
            return path
        if attempt == attempts:
            raise RuntimeError(f"{name}: download incomplete; rerun to resume {received}/{size}")
        print(f"DOWNLOAD {name}: resume {received}/{size}, attempt {attempt + 1}", flush=True)
        result = subprocess.run([
            "curl", "--fail", "--location", "--silent", "--show-error",
            "--continue-at", "-", "--connect-timeout", "20", "--max-time", "100",
            "--output", str(path), url,
        ])
        if result.returncode not in (0, 18, 28, 35, 56):
            raise RuntimeError(f"curl exited {result.returncode}; rerun after fixing connectivity")
    raise AssertionError("unreachable")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--quant", action="append", choices=["Q4_K_M", "Q5_K_M"],
                        help="model to download; repeat to obtain both (default: Q5_K_M)")
    parser.add_argument("--runtime-only", action="store_true")
    parser.add_argument("--attempts", type=int, default=10,
                        help="100-second download attempts per artifact")
    args = parser.parse_args()
    if args.attempts < 1:
        parser.error("--attempts must be positive")
    RUNTIME.mkdir(parents=True, exist_ok=True)
    packages = [name for name in ASSETS if name.endswith(".tar.gz")]
    selected = packages + ([] if args.runtime_only else [
        f"Qwen3.5-4B-{quant}.gguf" for quant in (args.quant or ["Q5_K_M"])
    ])
    verified = {}
    for name in selected:
        path = acquire(name, args.attempts)
        verified[name] = ASSETS[name][2]
        if name.endswith(".tar.gz"):
            (RUNTIME / "llama").mkdir(exist_ok=True)
            # Extract beside the installation and replace each file by rename.
            # A running server may still hold the old executable/library inode;
            # writing that inode in place would fail with ETXTBSY or corrupt it.
            with tempfile.TemporaryDirectory(prefix="extract-", dir=RUNTIME) as staging:
                with tarfile.open(path) as archive:
                    archive.extractall(staging, filter="data")
                source = Path(staging)
                for member in sorted(source.rglob("*")):
                    target = RUNTIME / "llama" / member.relative_to(source)
                    if member.is_dir() and not member.is_symlink():
                        target.mkdir(parents=True, exist_ok=True)
                    else:
                        target.parent.mkdir(parents=True, exist_ok=True)
                        os.replace(member, target)
    metadata = RUNTIME / "metadata"
    metadata.mkdir(exist_ok=True)
    manifest = dict(
        llama_tag=LLAMA_TAG, llama_commit=LLAMA_COMMIT, cuda_version="13.4",
        quant_repo="unsloth/Qwen3.5-4B-GGUF", quant_revision=GGUF_REVISION,
        original_repo="Qwen/Qwen3.5-4B", original_revision=SOURCE_REVISION,
        verified_sha256=verified,
    )
    (metadata / "installation.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Local runtime ready; see .runtime/metadata/installation.json", flush=True)


if __name__ == "__main__":
    main()
