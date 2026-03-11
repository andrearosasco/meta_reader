#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN=""

if [[ -x "$ROOT_DIR/.conda/bin/python" ]]; then
    PYTHON_BIN="$ROOT_DIR/.conda/bin/python"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python3)"
else
    echo "Python 3 is required to run the Quest receiver service." >&2
    exit 1
fi

exec "$PYTHON_BIN" "$ROOT_DIR/scripts/quest_teleop_receiver.py" "$@"