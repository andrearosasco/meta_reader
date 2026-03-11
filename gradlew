#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GRADLE_BIN="$SCRIPT_DIR/.conda/bin/gradle"
if [[ ! -x "$GRADLE_BIN" ]]; then
    echo "Missing workspace-local Gradle at $GRADLE_BIN" >&2
    exit 1
fi
exec "$GRADLE_BIN" -p "$SCRIPT_DIR" "$@"
