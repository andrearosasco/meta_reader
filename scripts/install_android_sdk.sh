#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK_DIR="${ANDROID_SDK_ROOT:-$ROOT_DIR/.android-sdk}"
TOOLS_VERSION="11076708"
TOOLS_ZIP="commandlinetools-linux-${TOOLS_VERSION}_latest.zip"
TOOLS_URL="https://dl.google.com/android/repository/${TOOLS_ZIP}"
CMDLINE_TOOLS_DIR="$SDK_DIR/cmdline-tools/latest"
LOCAL_PROPERTIES_FILE="$ROOT_DIR/local.properties"

mkdir -p "$SDK_DIR/cmdline-tools"

TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TEMP_DIR"' EXIT

if [[ ! -x "$CMDLINE_TOOLS_DIR/bin/sdkmanager" ]]; then
    echo "Downloading Android command-line tools into $SDK_DIR"
    curl -L --fail --output "$TEMP_DIR/$TOOLS_ZIP" "$TOOLS_URL"
    python - <<PY
import pathlib
import zipfile
zip_path = pathlib.Path(r"$TEMP_DIR/$TOOLS_ZIP")
out_dir = pathlib.Path(r"$TEMP_DIR/unpacked")
out_dir.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(zip_path) as archive:
    archive.extractall(out_dir)
PY
    rm -rf "$CMDLINE_TOOLS_DIR"
    mkdir -p "$CMDLINE_TOOLS_DIR"
    cp -R "$TEMP_DIR/unpacked/cmdline-tools/." "$CMDLINE_TOOLS_DIR/"
fi

export ANDROID_SDK_ROOT="$SDK_DIR"
export ANDROID_HOME="$SDK_DIR"

yes | "$CMDLINE_TOOLS_DIR/bin/sdkmanager" --sdk_root="$SDK_DIR" --licenses >/dev/null
"$CMDLINE_TOOLS_DIR/bin/sdkmanager" --sdk_root="$SDK_DIR" \
    "platform-tools" \
    "platforms;android-34" \
    "build-tools;34.0.0" \
    "cmake;3.22.1" \
    "ndk;26.3.11579264"

cat > "$LOCAL_PROPERTIES_FILE" <<EOF
sdk.dir=$SDK_DIR
EOF

echo "Android SDK installed in $SDK_DIR"
echo "Wrote $LOCAL_PROPERTIES_FILE"
