#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONDA_DIR="$ROOT_DIR/.conda"
GRADLEW="$ROOT_DIR/gradlew"

if [[ ! -x "$GRADLEW" ]]; then
    echo "Missing gradle wrapper at $GRADLEW" >&2
    exit 1
fi

if [[ ! -x "$CONDA_DIR/bin/java" ]]; then
    echo "Missing workspace-local Java at $CONDA_DIR/bin/java" >&2
    exit 1
fi

if [[ ! -f "$ROOT_DIR/local.properties" ]]; then
    echo "Missing $ROOT_DIR/local.properties. Run scripts/install_android_sdk.sh first." >&2
    exit 1
fi

export PATH="$CONDA_DIR/bin:$PATH"
export JAVA_HOME="$CONDA_DIR"

cd "$ROOT_DIR"
exec "$GRADLEW" assembleDebug --console plain "$@"