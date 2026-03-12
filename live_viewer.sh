#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_BIN=""

if [[ "${CONDA_DEFAULT_ENV:-}" == "websockets_working" ]] && command -v python >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python)"
elif command -v conda >/dev/null 2>&1 && conda env list | grep -q '^websockets_working[[:space:]]'; then
    PYTHON_BIN="conda run -n websockets_working python"
elif [[ -x "$ROOT_DIR/.conda/bin/python" ]]; then
    PYTHON_BIN="$ROOT_DIR/.conda/bin/python"
elif command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python3)"
else
    echo "Python 3 is required to run the MetaReader live pose viewer." >&2
    exit 1
fi

if [[ "$PYTHON_BIN" == "conda run -n websockets_working python" ]]; then
    exec conda run -n websockets_working python "$ROOT_DIR/scripts/live_pose_viewer.py" "$@"
fi

exec "$PYTHON_BIN" "$ROOT_DIR/scripts/live_pose_viewer.py" "$@"