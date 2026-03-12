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

If an authorized ADB-connected Quest is present, the receiver now switches to wired mode automatically, configures `adb reverse` for you, and does not advertise Avahi. If no ADB-connected Quest is present, it switches to wireless mode automatically and keeps the Avahi DNS-SD flow.

Wired mode:

```bash
./receiver.sh --port 5005 --tcp-port <HOST_PORT>
./install.sh --launch
```

When the Quest is connected through ADB/USB, the receiver configures `adb reverse tcp:5005 tcp:<HOST_PORT>` automatically, the app detects wired mode automatically, and the transport uses only the localhost TCP path at `127.0.0.1:5005`.

Wireless mode:

```bash
./receiver.sh --port 5005
./install.sh --launch
```

When the Quest is not connected through ADB/USB, the app detects wireless mode automatically, starts Avahi DNS-SD discovery, resolves the receiver on the LAN, and uses the wireless UDP path.

## Python API

You can also use the receiver as a small Python library instead of the logging CLI:

```python
from metareader import MetaReader

with MetaReader() as reader:
    frame = reader.read()
    print(frame.head_pose.position)
    print(frame.left_hand.fingertips["index_tip"].pose)
```

`MetaReader.read()` returns a parsed frame with:

- `head_pose`
- `left_hand`
- `right_hand`
- `status`
- `raw`

The same class automatically chooses wired mode with `adb reverse` when an authorized ADB-connected Quest is present, otherwise it uses the wireless Avahi path.

## Live Viewer

To see the head and hand positions moving live in a 3D window:

```bash
cd /home/panda-admin/users/arosasco/meta_reader
./live_viewer.sh
```

The viewer uses `MetaReader` directly, so it follows the same automatic mode selection as the receiver CLI: wired with automatic `adb reverse` when ADB is connected, otherwise wireless with Avahi.

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
