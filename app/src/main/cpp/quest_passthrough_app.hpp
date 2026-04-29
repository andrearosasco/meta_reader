#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android_native_app_glue.h>
#include <openxr/openxr_platform.h>

#include "transport_telemetry.hpp"

#include <array>
#include <string>
#include <vector>

class QuestPassthroughApp {
public:
    explicit QuestPassthroughApp(android_app* app);
    ~QuestPassthroughApp();

    void HandleAppCommand(int32_t cmd);
    void RunMainLoop();

private:
    struct HandJointVisualState;
    struct HandOverlayState;

    bool Initialize();
    bool InitializeEgl();
    bool InitializeOpenXr();
    void Shutdown();
    void ShutdownOpenXr();
    void ShutdownEgl();
    void PollAndroidEvents(bool* shouldExit);
    void PollXrEvents(bool* shouldExit);
    void HandleSessionStateChanged(const XrEventDataSessionStateChanged& stateChangedEvent, bool* shouldExit);
    bool RenderFrame();
    bool RefreshHudTexture();
    HmdPoseState LocateHeadPose(XrTime predictedDisplayTime);
    void LocateHandJoints(XrTime predictedDisplayTime);
    HandTelemetryState BuildHandTelemetry(const HandOverlayState& handState) const;
    void AppendHandLayers(
        const HandOverlayState& handState,
        std::vector<XrCompositionLayerQuad>* handLayers,
        std::vector<XrCompositionLayerBaseHeader*>* layers) const;
    void UpdateTelemetry(XrTime predictedDisplayTime, const HmdPoseState& headPose);

    struct HandJointVisualState {
        XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
        float radius{0.02f};
        bool tracked{false};
    };

    struct HandOverlayState {
        XrHandTrackerEXT tracker{XR_NULL_HANDLE};
        bool tracked{false};
        std::array<HandJointVisualState, XR_HAND_JOINT_COUNT_EXT> joints{};
    };

    template <typename T>
    bool GetInstanceProc(const char* functionName, T* function) {
        return XR_SUCCEEDED(xrGetInstanceProcAddr(
            instance_, functionName, reinterpret_cast<PFN_xrVoidFunction*>(function)));
    }

    android_app* app_{nullptr};
    bool initialized_{false};
    bool sessionRunning_{false};
    bool exitRequested_{false};

    EGLDisplay eglDisplay_{EGL_NO_DISPLAY};
    EGLConfig eglConfig_{nullptr};
    EGLContext eglContext_{EGL_NO_CONTEXT};
    EGLSurface eglSurface_{EGL_NO_SURFACE};

    XrInstance instance_{XR_NULL_HANDLE};
    XrSystemId systemId_{XR_NULL_SYSTEM_ID};
    XrSession session_{XR_NULL_HANDLE};
    XrSpace appSpace_{XR_NULL_HANDLE};
    XrSpace viewSpace_{XR_NULL_HANDLE};
    XrPassthroughFB passthrough_{XR_NULL_HANDLE};
    XrPassthroughLayerFB passthroughLayer_{XR_NULL_HANDLE};

    XrSwapchain hudSwapchain_{XR_NULL_HANDLE};
    uint32_t hudWidth_{1024};
    uint32_t hudHeight_{192};
    std::vector<XrSwapchainImageOpenGLESKHR> hudImages_;
    bool hudTextureInitialized_{false};
    std::string lastHudSnapshot_;

    XrSwapchain handOverlaySwapchain_{XR_NULL_HANDLE};
    uint32_t handOverlayWidth_{128};
    uint32_t handOverlayHeight_{128};
    std::vector<XrSwapchainImageOpenGLESKHR> handOverlayImages_;
    HandOverlayState leftHandOverlay_;
    HandOverlayState rightHandOverlay_;

    PFN_xrGetOpenGLESGraphicsRequirementsKHR xrGetOpenGLESGraphicsRequirementsKHR_{nullptr};
    PFN_xrCreatePassthroughFB xrCreatePassthroughFB_{nullptr};
    PFN_xrDestroyPassthroughFB xrDestroyPassthroughFB_{nullptr};
    PFN_xrPassthroughStartFB xrPassthroughStartFB_{nullptr};
    PFN_xrPassthroughPauseFB xrPassthroughPauseFB_{nullptr};
    PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB_{nullptr};
    PFN_xrDestroyPassthroughLayerFB xrDestroyPassthroughLayerFB_{nullptr};
    PFN_xrPassthroughLayerResumeFB xrPassthroughLayerResumeFB_{nullptr};
    PFN_xrPassthroughLayerPauseFB xrPassthroughLayerPauseFB_{nullptr};
    PFN_xrCreateHandTrackerEXT xrCreateHandTrackerEXT_{nullptr};
    PFN_xrDestroyHandTrackerEXT xrDestroyHandTrackerEXT_{nullptr};
    PFN_xrLocateHandJointsEXT xrLocateHandJointsEXT_{nullptr};

    TransportTelemetryBridge transport_{};
};
