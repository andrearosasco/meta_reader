#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android_native_app_glue.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class QuestPassthroughApp {
public:
    explicit QuestPassthroughApp(android_app* app);
    ~QuestPassthroughApp();

    void HandleAppCommand(int32_t cmd);
    void RunMainLoop();

private:
    struct TelemetryState {
        std::string connectionState{"DISCOVERING"};
        float packetRate{0.0f};
        bool trackingValid{false};
        std::string localIp{"LOOKUP"};
        std::string targetHost{"SEARCHING"};
    };

    struct HmdPoseState {
        bool valid{false};
        XrVector3f position{0.0f, 0.0f, 0.0f};
        XrQuaternionf orientation{0.0f, 0.0f, 0.0f, 1.0f};
    };

    struct TransportSelection {
        bool wiredMode{false};
        bool hasEndpoint{false};
        std::string host;
        uint16_t port{0};
    };

    bool Initialize();
    bool InitializeEgl();
    bool InitializeOpenXr();
    bool InitializeSpaces();
    bool InitializePassthrough();
    bool InitializeHudSwapchain();
    bool InitializeHandTracking();
    bool InitializeHandOverlaySwapchain();
    bool ChooseEnvironmentBlendMode();
    void Shutdown();
    void ShutdownOpenXr();
    void ShutdownEgl();
    void PollAndroidEvents(bool* shouldExit);
    void PollXrEvents(bool* shouldExit);
    void HandleSessionStateChanged(const XrEventDataSessionStateChanged& stateChangedEvent, bool* shouldExit);
    bool RenderFrame();
    void UpdateTelemetry(XrTime predictedDisplayTime);
    void UpdateTransportTelemetry(const TransportSelection& selection);
    TransportSelection ReadTransportSelection() const;
    void SyncTransportConnection(const TransportSelection& selection);
    bool OpenTransportConnection(const TransportSelection& selection);
    void CloseTransportConnection();
    bool SendTelemetryPacket(const std::vector<uint8_t>& packet);
    std::vector<uint8_t> SerializeTelemetryPacket(
        const TransportSelection& selection,
        XrTime predictedDisplayTime,
        const HmdPoseState& headPose);
    void NoteSuccessfulPacketSend();
    void RenderHudToSwapchain(uint32_t imageIndex);
    void RenderHandOverlayToSwapchain(uint32_t imageIndex);
    std::string MakeHudSnapshot() const;
    std::string DetectLocalIp() const;
    bool IsInstanceExtensionSupported(const char* extensionName) const;

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
    bool resumed_{false};
    bool sessionRunning_{false};
    bool shouldRender_{false};
    bool exitRequested_{false};
    XrSessionState sessionState_{XR_SESSION_STATE_UNKNOWN};

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
    XrEnvironmentBlendMode environmentBlendMode_{XR_ENVIRONMENT_BLEND_MODE_OPAQUE};

    XrSwapchain hudSwapchain_{XR_NULL_HANDLE};
    int64_t hudSwapchainFormat_{0};
    uint32_t hudWidth_{1536};
    uint32_t hudHeight_{288};
    std::vector<XrSwapchainImageOpenGLESKHR> hudImages_;
    bool hudTextureInitialized_{false};
    std::string lastHudSnapshot_;

    bool handTrackingSupported_{false};
    XrSwapchain handOverlaySwapchain_{XR_NULL_HANDLE};
    uint32_t handOverlayWidth_{128};
    uint32_t handOverlayHeight_{128};
    std::vector<XrSwapchainImageOpenGLESKHR> handOverlayImages_;
    bool handOverlayTextureInitialized_{false};
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

    std::vector<std::string> availableInstanceExtensions_;
    TelemetryState telemetry_;
    int transportSocket_{-1};
    bool transportSocketConnected_{false};
    bool transportSocketWiredMode_{false};
    std::string transportHost_;
    uint16_t transportPort_{0};
    uint64_t telemetrySequence_{0};
    uint32_t packetWindowCount_{0};
    std::chrono::steady_clock::time_point packetWindowStart_{};
    std::chrono::steady_clock::time_point lastSuccessfulSendTime_{};
    std::chrono::steady_clock::time_point nextReconnectAttempt_{};
};
