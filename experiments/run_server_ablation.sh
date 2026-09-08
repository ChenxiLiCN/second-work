#!/usr/bin/env bash
# Run from any directory. Additional arguments are forwarded to the runner.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
command -v python3 >/dev/null || { echo "python3 is required"; exit 1; }
test -x /usr/bin/time || { echo "Install the Linux time package"; exit 1; }
command -v timeout >/dev/null || { echo "timeout is required"; exit 1; }
exec python3 "$SCRIPT_DIR/run_server_ablation.py" "$@"
