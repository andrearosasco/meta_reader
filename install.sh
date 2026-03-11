#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APK_PATH="$ROOT_DIR/app/build/outputs/apk/debug/app-debug.apk"
PACKAGE_NAME="com.example.metareader"
ACTIVITY_NAME="com.example.metareader.MetaReaderActivity"

build_first=false
launch_after_install=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build)
            build_first=true
            ;;
        --launch)
            launch_after_install=true
            ;;
        -h|--help)
            cat <<'EOF'
Usage: ./install.sh [--build] [--launch]

  --build   Build the debug APK before installing it.
  --launch  Launch the app after installing it.
EOF
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
    shift
done

if [[ "$build_first" == true ]]; then
    "$ROOT_DIR/build.sh"
fi

if [[ ! -f "$APK_PATH" ]]; then
    echo "Missing APK at $APK_PATH" >&2
    echo "Run ./build.sh first or use ./install.sh --build" >&2
    exit 1
fi

if ! command -v adb >/dev/null 2>&1; then
    echo "adb is not in PATH" >&2
    exit 1
fi

if ! adb get-state >/dev/null 2>&1; then
    echo "No authorized ADB device detected" >&2
    exit 1
fi

adb install -r "$APK_PATH"

if [[ "$launch_after_install" == true ]]; then
    adb shell am start \
        -n "$PACKAGE_NAME/$ACTIVITY_NAME" \
        -a android.intent.action.MAIN \
        -c org.khronos.openxr.intent.category.IMMERSIVE_HMD
fi