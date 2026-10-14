#!/usr/bin/env bash
# Serialize startup, reuse a compatible healthy local model, or publish the
# real free port bound by llama-server itself. No system driver is installed.
set -euo pipefail
TASK4_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
exec python3 "$TASK4_ROOT/scripts/model/launch.py" "$@"
