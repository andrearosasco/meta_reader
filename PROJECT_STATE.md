# Project State

## Goal
Build a native Quest Android app in C++ using OpenXR with `XR_FB_passthrough` enabled and only a minimal HUD overlay.

## Status
- Planning started on 2026-03-11.
- Workspace was initially empty.
- Android/Gradle/CMake scaffold is in place.
- Native OpenXR passthrough implementation is in place.
- Workspace-local Android SDK/NDK is installed in `.android-sdk/`.
- Local debug build succeeded and produced `app/build/outputs/apk/debug/app-debug.apk`.
- Quest launch is verified over ADB.
- Runtime loader initialization is fixed by resolving `xrInitializeLoaderKHR` through `xrGetInstanceProcAddr` first.
- Quest launch gating is fixed by adding Quest hand-tracking/focus-aware manifest metadata.
- Passthrough frame submission is validated on-device after removing the invalid passthrough-layer `space` and preferring `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND`.
- Quest-side DNS-SD discovery is wired through a custom `NativeActivity` subclass using Android `NsdManager`.
- Linux-side receiver discovery is supported by `receiver.sh` / `scripts/quest_teleop_receiver.py` using Avahi when available.
- Quest-side transport selection now uses USB/ADB state: wired ADB-reverse TCP when cable-connected, otherwise wireless Avahi DNS-SD + UDP.
- Linux-side receiver now accepts the same framed telemetry packets over both wireless UDP and wired TCP.

## Active Todos
1. Visually confirm Quest-side DNS-SD discovery against the Linux receiver announcer
2. Capture runtime screenshots and video from Quest
3. Replace placeholder UDP telemetry payloads with real pose/button transport

## Notes
- User requested no virtual room rendering.
- User requested environment installation without sudo; prefer conda-managed tooling where practical.
- APK, screenshots, and video depend on Quest hardware / Android SDK availability.
- Current source layout includes `app/build.gradle`, `app/src/main/AndroidManifest.xml`, and native sources under `app/src/main/cpp/`.
- HUD currently uses real tracking validity and local IP discovery, with placeholder connection/session telemetry by design.
- Verified runtime command: `adb shell am start -n com.example.metareader/android.app.NativeActivity -a android.intent.action.MAIN -c org.khronos.openxr.intent.category.IMMERSIVE_HMD`.
- Verified focused logs: `adb logcat -s MetaReaderXR OpenXR AndroidRuntime RuntimeTelemetryThread`.
