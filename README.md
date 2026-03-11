# Meta Reader Quest Passthrough App

## Install

Host requirements:

- `bash`
- `curl`
- `python3`
- Meta Quest in Developer Mode
- optional: `avahi-publish-service` for receiver auto-discovery advertisement

From the repo root:

```bash
cd /home/panda-admin/users/arosasco/meta_reader
bash scripts/install_android_sdk.sh
export PATH="$PWD/.android-sdk/platform-tools:$PATH"
```

This installs the Android SDK/NDK locally into `.android-sdk/` and writes `local.properties`.

## Run

Build, install, and launch the Quest app:

```bash
cd /home/panda-admin/users/arosasco/meta_reader
export PATH="$PWD/.android-sdk/platform-tools:$PATH"
./install.sh --build --launch
```

Run the Linux receiver:

```bash
cd /home/panda-admin/users/arosasco/meta_reader
./receiver.sh --port 5005
```

If you want the steps split out:

```bash
./build.sh
./install.sh
./install.sh --launch
```

## Debug

Check device connection:

```bash
adb devices
```

Watch logs:

```bash
adb logcat -s MetaReaderXR OpenXR AndroidRuntime RuntimeTelemetryThread
```

Clear logs and relaunch cleanly:

```bash
adb logcat -c
adb shell am force-stop com.example.metareader
./install.sh --launch
```

Manual launch:

```bash
adb shell am start \
    -n com.example.metareader/com.example.metareader.MetaReaderActivity \
    -a android.intent.action.MAIN \
    -c org.khronos.openxr.intent.category.IMMERSIVE_HMD
```

Reinstall current APK manually:

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```
