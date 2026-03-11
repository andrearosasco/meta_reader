# Meta Reader Quest Passthrough App

Native Meta Quest Android app in `C++` using OpenXR with `XR_FB_passthrough` enabled and a minimal HUD overlay.

## What is implemented

- Native `NativeActivity` Android app with Gradle + CMake
- OpenXR Android loader initialization via `XR_KHR_loader_init_android`
- OpenGL ES session creation via `XR_KHR_opengl_es_enable`
- Quest passthrough lifecycle via `XR_FB_passthrough`
- Minimal head-locked HUD quad showing:
  - connection state
  - packet rate
  - tracking valid/invalid
  - local IP
  - target host
- No virtual room or world geometry

## Current HUD behavior

The HUD already uses real OpenXR tracking state and real local IPv4 discovery.
Connection state and target host now reflect local-network DNS-SD discovery for `_quest-teleop._udp` services.
Packet rate is still placeholder telemetry because packet transport is explicitly out of scope for this phase.

## Workspace-local Android setup

The repo includes `scripts/install_android_sdk.sh` to install the Android SDK and NDK without `sudo`.

### 1. Install the Android SDK locally

```bash
cd /home/panda-admin/users/arosasco/meta_reader
bash scripts/install_android_sdk.sh
```

This installs into `.android-sdk/` and writes `local.properties` automatically.

### 2. Use the local conda toolchain

The repo already contains a workspace-local toolchain in `.conda/`.
When building manually, prefer:

```bash
export PATH="/home/panda-admin/users/arosasco/meta_reader/.conda/bin:$PATH"
```

## Build

From the repo root:

```bash
./build.sh
```

Expected APK path:

```text
app/build/outputs/apk/debug/app-debug.apk
```

## Install on Quest

With developer mode enabled and the headset connected via ADB:

```bash
./install.sh
./install.sh --launch
```

## Automatic receiver discovery

Run the Linux-side receiver and DNS-SD announcer with:

```bash
./receiver.sh --port 5005
```

This binds the UDP port locally and, when `avahi-publish-service` is available, advertises a `_quest-teleop._udp` service with these TXT keys:

- `proto=1`
- `ros_domain_id=0`
- `node=quest_bridge`
- `caps=pose,buttons,hmd`

On Quest, the app now uses Android `NsdManager` to discover and resolve `_quest-teleop._udp.` services on the local network. The HUD updates `CONNECTION` and `TARGET HOST` automatically when a receiver is resolved.

The launch activity is `com.example.metareader.MetaReaderActivity`; `./install.sh --launch` uses that automatically.

## Runtime logs

Capture logs with:

```bash
adb logcat -s MetaReaderXR OpenXR AndroidRuntime RuntimeTelemetryThread
```

## Verified runtime fixes

- Quest Shell launch gating is addressed by declaring optional hand tracking and focus-aware metadata in `app/src/main/AndroidManifest.xml`.
- OpenXR loader initialization now resolves `xrInitializeLoaderKHR` via `xrGetInstanceProcAddr` before falling back to dynamic symbol lookup.
- Passthrough frame submission uses a null `space` on `XrCompositionLayerPassthroughFB` and prefers `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND`.

## Validation notes

- Passthrough requires Quest hardware and the `com.oculus.feature.PASSTHROUGH` feature.
- The project is configured for `arm64-v8a` only.
- The app intentionally submits only passthrough and a HUD quad layer.
- If the `openxr_loader_for_android` Maven artifact version changes, update `openxrLoaderVersion` in `gradle.properties`.

## Likely next steps

- Replace placeholder telemetry with real network/session state
- Add device-side screenshots/video capture
- Capture and archive runtime logs from Quest validation runs
