#!/usr/bin/env python3
"""Compile and run real filesystem/flock checks in an isolated private root."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="task4-session-unit-") as temporary:
        binary = Path(temporary) / "sessions-unit"
        compiler = shlex.split(os.environ.get("CC", "gcc"))
        subprocess.run(
            compiler + ["-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", "-Iinclude",
                        "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O2", "-g",
                        "tests/client_sessions_unit.c", "src/client/state.c",
                        "src/storage/checkpoint.c", "src/storage/files.c", "-o", str(binary)],
            cwd=ROOT, check=True,
        )
        subprocess.run([str(binary), temporary], cwd=ROOT, check=True, timeout=15)


if __name__ == "__main__":
    main()
