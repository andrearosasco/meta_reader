#include "quest_passthrough_app.hpp"
#include "hud_renderer.hpp"

#include <jni.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

namespace {

constexpr const char* kLogTag = "MetaReaderXR";

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

void LogError(const std::string& message) {
    __android_log_write(ANDROID_LOG_ERROR, kLogTag, message.c_str());
}

std::string JStringToUtf8(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* utfChars = env->GetStringUTFChars(value, nullptr);
    if (utfChars == nullptr) {
        return {};
    }
    std::string text(utfChars);
    env->ReleaseStringUTFChars(value, utfChars);
    return text;
}

void LogXrResult(XrInstance instance, const char* context, XrResult result) {
    std::ostringstream stream;
    stream << context << " failed with ";
    if (instance != XR_NULL_HANDLE) {
        char resultString[XR_MAX_RESULT_STRING_SIZE]{};
        if (XR_SUCCEEDED(xrResultToString(instance, result, resultString))) {
            stream << resultString;
        } else {
            stream << static_cast<int>(result);
        }
    } else {
        stream << static_cast<int>(result);
    }
    LogError(stream.str());
}

XrPosef IdentityPose() {
    XrPosef pose{};
    pose.orientation.w = 1.0f;
    return pose;
}

constexpr std::array<XrHandJointEXT, 7> kHandOverlayJoints{
    XR_HAND_JOINT_WRIST_EXT,
    XR_HAND_JOINT_PALM_EXT,
    XR_HAND_JOINT_THUMB_TIP_EXT,
    XR_HAND_JOINT_INDEX_TIP_EXT,
    XR_HAND_JOINT_MIDDLE_TIP_EXT,
    XR_HAND_JOINT_RING_TIP_EXT,
    XR_HAND_JOINT_LITTLE_TIP_EXT,
};

constexpr std::array<std::pair<XrHandJointEXT, const char*>, 5> kFingertipJoints{{
    {XR_HAND_JOINT_THUMB_TIP_EXT, "thumb_tip"},
    {XR_HAND_JOINT_INDEX_TIP_EXT, "index_tip"},
    {XR_HAND_JOINT_MIDDLE_TIP_EXT, "middle_tip"},
    {XR_HAND_JOINT_RING_TIP_EXT, "ring_tip"},
    {XR_HAND_JOINT_LITTLE_TIP_EXT, "little_tip"},
}};

bool CreateColorSwapchain(
    XrSession session,
    XrInstance instance,
    uint32_t width,
    uint32_t height,
    const char* label,
    XrSwapchain* swapchain,
    std::vector<XrSwapchainImageOpenGLESKHR>* images) {
    uint32_t formatCount = 0;
    XrResult xrResult = xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance, (std::string("xrEnumerateSwapchainFormats(") + label + " count)").c_str(), xrResult);
        return false;
    }

    std::vector<int64_t> formats(formatCount);
    xrResult = xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance, (std::string("xrEnumerateSwapchainFormats(") + label + " list)").c_str(), xrResult);
        return false;
    }

    const std::array<int64_t, 2> preferredFormats{GL_SRGB8_ALPHA8, GL_RGBA8};
    const auto formatIt = std::find_first_of(formats.begin(), formats.end(), preferredFormats.begin(), preferredFormats.end());
    const int64_t format = formatIt != formats.end() ? *formatIt : formats.front();

    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.format = format;
    createInfo.sampleCount = 1;
    createInfo.width = width;
    createInfo.height = height;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

    xrResult = xrCreateSwapchain(session, &createInfo, swapchain);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance, (std::string("xrCreateSwapchain(") + label + ")").c_str(), xrResult);
        return false;
    }

    uint32_t imageCount = 0;
    xrResult = xrEnumerateSwapchainImages(*swapchain, 0, &imageCount, nullptr);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance, (std::string("xrEnumerateSwapchainImages(") + label + " count)").c_str(), xrResult);
        return false;
    }

    images->assign(imageCount, XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrResult = xrEnumerateSwapchainImages(
        *swapchain,
        imageCount,
        &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(images->data()));
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance, (std::string("xrEnumerateSwapchainImages(") + label + " list)").c_str(), xrResult);
        return false;
    }
    return true;
}

bool AcquireSwapchainImageBlocking(XrInstance instance, XrSwapchain swapchain, const char* label, uint32_t* imageIndex) {
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult xrResult = xrAcquireSwapchainImage(swapchain, &acquireInfo, imageIndex);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance, (std::string("xrAcquireSwapchainImage(") + label + ")").c_str(), xrResult);
        return false;
    }

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    xrResult = xrWaitSwapchainImage(swapchain, &waitInfo);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance, (std::string("xrWaitSwapchainImage(") + label + ")").c_str(), xrResult);
        return false;
    }
    return true;
}

bool ReleaseSwapchainImageChecked(XrInstance instance, XrSwapchain swapchain, const char* label) {
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult xrResult = xrReleaseSwapchainImage(swapchain, &releaseInfo);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance, (std::string("xrReleaseSwapchainImage(") + label + ")").c_str(), xrResult);
        return false;
    }
    return true;
}

}  // namespace

QuestPassthroughApp::QuestPassthroughApp(android_app* app) : app_(app) {}

QuestPassthroughApp::~QuestPassthroughApp() {
    Shutdown();
}

void QuestPassthroughApp::HandleAppCommand(int32_t cmd) {
    exitRequested_ = exitRequested_ || cmd == APP_CMD_DESTROY;
}

void QuestPassthroughApp::RunMainLoop() {
    if (!Initialize()) {
        LogError("Initialization failed; exiting main loop.");
        return;
    }

    while (!exitRequested_) {
        PollAndroidEvents(&exitRequested_);
        if (exitRequested_) {
            break;
        }

        PollXrEvents(&exitRequested_);
        if (exitRequested_) {
            break;
        }

        if (sessionRunning_) {
            RenderFrame();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
}

bool QuestPassthroughApp::Initialize() {
    if (initialized_) {
        return true;
    }

    if (!InitializeEgl()) {
        return false;
    }

    if (!InitializeOpenXr()) {
        ShutdownEgl();
        return false;
    }
    initialized_ = true;
    return true;
}

bool QuestPassthroughApp::InitializeEgl() {
    eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay_ == EGL_NO_DISPLAY) {
        LogError("eglGetDisplay returned EGL_NO_DISPLAY.");
        return false;
    }

    if (!eglInitialize(eglDisplay_, nullptr, nullptr)) {
        LogError("eglInitialize failed.");
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        LogError("eglBindAPI failed.");
        return false;
    }

    const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE,
    };

    EGLint numConfigs = 0;
    if (!eglChooseConfig(eglDisplay_, configAttributes, &eglConfig_, 1, &numConfigs) || numConfigs != 1) {
        LogError("eglChooseConfig failed.");
        return false;
    }

    const EGLint pbufferAttributes[] = {
        EGL_WIDTH, 16,
        EGL_HEIGHT, 16,
        EGL_NONE,
    };
    eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufferAttributes);
    if (eglSurface_ == EGL_NO_SURFACE) {
        LogError("eglCreatePbufferSurface failed.");
        return false;
    }

    const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttributes);
    if (eglContext_ == EGL_NO_CONTEXT) {
        LogError("eglCreateContext failed.");
        return false;
    }

    if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
        LogError("eglMakeCurrent failed.");
        return false;
    }

    return true;
}

bool QuestPassthroughApp::InitializeOpenXr() {
    using PfnInitializeLoader = XrResult(XRAPI_PTR*)(const XrLoaderInitInfoBaseHeaderKHR* loaderInitInfo);
    PFN_xrVoidFunction loaderInitVoidFunction = nullptr;
    const XrResult loaderProcResult = xrGetInstanceProcAddr(
        XR_NULL_HANDLE,
        "xrInitializeLoaderKHR",
        &loaderInitVoidFunction);
    if (XR_FAILED(loaderProcResult) || loaderInitVoidFunction == nullptr) {
        LogError("Failed to resolve xrInitializeLoaderKHR.");
        return false;
    }
    const auto xrInitializeLoaderKHRFn = reinterpret_cast<PfnInitializeLoader>(loaderInitVoidFunction);

    XrLoaderInitInfoAndroidKHR loaderInitInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInitInfo.applicationVM = app_->activity->vm;
    loaderInitInfo.applicationContext = app_->activity->clazz;

    const XrResult loaderResult = xrInitializeLoaderKHRFn(
        reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInitInfo));
    if (XR_FAILED(loaderResult)) {
        LogXrResult(XR_NULL_HANDLE, "xrInitializeLoaderKHR", loaderResult);
        return false;
    }

    const std::array<const char*, 4> enabledExtensions{
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_FB_PASSTHROUGH_EXTENSION_NAME,
        XR_EXT_HAND_TRACKING_EXTENSION_NAME,
    };

    XrInstanceCreateInfoAndroidKHR androidCreateInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidCreateInfo.applicationVM = app_->activity->vm;
    androidCreateInfo.applicationActivity = app_->activity->clazz;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidCreateInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    createInfo.enabledExtensionNames = enabledExtensions.data();
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    std::snprintf(createInfo.applicationInfo.applicationName,
                  XR_MAX_APPLICATION_NAME_SIZE,
                  "%s",
                  "MetaReader");
    std::snprintf(createInfo.applicationInfo.engineName,
                  XR_MAX_ENGINE_NAME_SIZE,
                  "%s",
                  "NativeOpenXR");
    createInfo.applicationInfo.applicationVersion = 1;
    createInfo.applicationInfo.engineVersion = 1;

    XrResult xrResult = xrCreateInstance(&createInfo, &instance_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(XR_NULL_HANDLE, "xrCreateInstance", xrResult);
        return false;
    }

    if (!GetInstanceProc("xrGetOpenGLESGraphicsRequirementsKHR", &xrGetOpenGLESGraphicsRequirementsKHR_) ||
        !GetInstanceProc("xrCreatePassthroughFB", &xrCreatePassthroughFB_) ||
        !GetInstanceProc("xrDestroyPassthroughFB", &xrDestroyPassthroughFB_) ||
        !GetInstanceProc("xrPassthroughStartFB", &xrPassthroughStartFB_) ||
        !GetInstanceProc("xrPassthroughPauseFB", &xrPassthroughPauseFB_) ||
        !GetInstanceProc("xrCreatePassthroughLayerFB", &xrCreatePassthroughLayerFB_) ||
        !GetInstanceProc("xrDestroyPassthroughLayerFB", &xrDestroyPassthroughLayerFB_) ||
        !GetInstanceProc("xrPassthroughLayerResumeFB", &xrPassthroughLayerResumeFB_) ||
        !GetInstanceProc("xrPassthroughLayerPauseFB", &xrPassthroughLayerPauseFB_)) {
        LogError("Failed to resolve one or more required OpenXR function pointers.");
        return false;
    }

    if (!GetInstanceProc("xrCreateHandTrackerEXT", &xrCreateHandTrackerEXT_) ||
        !GetInstanceProc("xrDestroyHandTrackerEXT", &xrDestroyHandTrackerEXT_) ||
        !GetInstanceProc("xrLocateHandJointsEXT", &xrLocateHandJointsEXT_)) {
        LogError("Failed to resolve XR_EXT_hand_tracking function pointers.");
        return false;
    }

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    xrResult = xrGetSystem(instance_, &systemInfo, &systemId_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrGetSystem", xrResult);
        return false;
    }

    XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    xrResult = xrGetOpenGLESGraphicsRequirementsKHR_(instance_, systemId_, &graphicsRequirements);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrGetOpenGLESGraphicsRequirementsKHR", xrResult);
        return false;
    }

    XrGraphicsBindingOpenGLESAndroidKHR graphicsBinding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphicsBinding.display = eglDisplay_;
    graphicsBinding.config = eglConfig_;
    graphicsBinding.context = eglContext_;

    XrSessionCreateInfo sessionCreateInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.next = &graphicsBinding;
    sessionCreateInfo.systemId = systemId_;
    xrResult = xrCreateSession(instance_, &sessionCreateInfo, &session_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrCreateSession", xrResult);
        return false;
    }

    const auto createSpace = [&](XrReferenceSpaceType type, const char* label, XrSpace* space) {
        XrReferenceSpaceCreateInfo createInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        createInfo.referenceSpaceType = type;
        createInfo.poseInReferenceSpace = IdentityPose();
        const XrResult result = xrCreateReferenceSpace(session_, &createInfo, space);
        if (XR_FAILED(result)) {
            LogXrResult(instance_, (std::string("xrCreateReferenceSpace(") + label + ")").c_str(), result);
            return false;
        }
        return true;
    };
    if (!createSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, "local", &appSpace_) ||
        !createSpace(XR_REFERENCE_SPACE_TYPE_VIEW, "view", &viewSpace_)) {
        return false;
    }

    XrPassthroughCreateInfoFB passthroughCreateInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    passthroughCreateInfo.flags = 0;

    xrResult = xrCreatePassthroughFB_(session_, &passthroughCreateInfo, &passthrough_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrCreatePassthroughFB", xrResult);
        return false;
    }

    XrPassthroughLayerCreateInfoFB passthroughLayerCreateInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    passthroughLayerCreateInfo.passthrough = passthrough_;
    passthroughLayerCreateInfo.flags = 0;
    passthroughLayerCreateInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;

    xrResult = xrCreatePassthroughLayerFB_(session_, &passthroughLayerCreateInfo, &passthroughLayer_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrCreatePassthroughLayerFB", xrResult);
        return false;
    }

    xrResult = xrPassthroughStartFB_(passthrough_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrPassthroughStartFB", xrResult);
        return false;
    }

    xrResult = xrPassthroughLayerResumeFB_(passthroughLayer_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrPassthroughLayerResumeFB", xrResult);
        return false;
    }

    if (!CreateColorSwapchain(session_, instance_, hudWidth_, hudHeight_, "hud", &hudSwapchain_, &hudImages_)) {
        return false;
    }

    const auto createTracker = [&](XrHandEXT hand, XrHandTrackerEXT* tracker) {
        XrHandTrackerCreateInfoEXT createInfo{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        createInfo.hand = hand;
        const XrResult result = xrCreateHandTrackerEXT_(session_, &createInfo, tracker);
        if (XR_FAILED(result)) {
            LogXrResult(instance_, hand == XR_HAND_LEFT_EXT ? "xrCreateHandTrackerEXT(left)" : "xrCreateHandTrackerEXT(right)", result);
            return false;
        }
        return true;
    };
    if (!createTracker(XR_HAND_LEFT_EXT, &leftHandOverlay_.tracker) ||
        !createTracker(XR_HAND_RIGHT_EXT, &rightHandOverlay_.tracker) ||
        !CreateColorSwapchain(session_, instance_, handOverlayWidth_, handOverlayHeight_, "hand overlay", &handOverlaySwapchain_, &handOverlayImages_)) {
        return false;
    }

    uint32_t imageIndex = 0;
    if (!AcquireSwapchainImageBlocking(instance_, handOverlaySwapchain_, "hand init", &imageIndex)) {
        return false;
    }
    RenderHandOverlayTexture(handOverlayImages_[imageIndex].image, handOverlayWidth_, handOverlayHeight_);
    if (!ReleaseSwapchainImageChecked(instance_, handOverlaySwapchain_, "hand init")) {
        return false;
    }
    return true;
}

void QuestPassthroughApp::PollAndroidEvents(bool* shouldExit) {
    int timeoutMs = sessionRunning_ ? 0 : 50;
    int events = 0;
    android_poll_source* source = nullptr;

    while (ALooper_pollAll(timeoutMs, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
        timeoutMs = 0;
        if (source != nullptr) {
            source->process(app_, source);
        }
        if (app_->destroyRequested != 0) {
            *shouldExit = true;
            return;
        }
    }
}

void QuestPassthroughApp::PollXrEvents(bool* shouldExit) {
    XrEventDataBuffer eventBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    while (instance_ != XR_NULL_HANDLE) {
        const XrResult xrResult = xrPollEvent(instance_, &eventBuffer);
        if (xrResult == XR_EVENT_UNAVAILABLE) {
            return;
        }
        if (XR_FAILED(xrResult)) {
            LogXrResult(instance_, "xrPollEvent", xrResult);
            *shouldExit = true;
            return;
        }

        switch (eventBuffer.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto* stateChangedEvent =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&eventBuffer);
                HandleSessionStateChanged(*stateChangedEvent, shouldExit);
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                *shouldExit = true;
                break;
            default:
                break;
        }

        eventBuffer = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void QuestPassthroughApp::HandleSessionStateChanged(
    const XrEventDataSessionStateChanged& stateChangedEvent,
    bool* shouldExit) {
    switch (stateChangedEvent.state) {
        case XR_SESSION_STATE_READY: {
            if (!sessionRunning_) {
                XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult xrResult = xrBeginSession(session_, &beginInfo);
                if (XR_FAILED(xrResult)) {
                    LogXrResult(instance_, "xrBeginSession", xrResult);
                    *shouldExit = true;
                    return;
                }
                sessionRunning_ = true;
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            if (sessionRunning_) {
                xrEndSession(session_);
                sessionRunning_ = false;
            }
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            *shouldExit = true;
            break;
        default:
            break;
    }
}

bool QuestPassthroughApp::RenderFrame() {
    XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrResult xrResult = xrWaitFrame(session_, &frameWaitInfo, &frameState);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrWaitFrame", xrResult);
        return false;
    }

    XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    xrResult = xrBeginFrame(session_, &frameBeginInfo);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrBeginFrame", xrResult);
        return false;
    }

    std::vector<XrCompositionLayerBaseHeader*> layers;
    std::vector<XrCompositionLayerQuad> handLayers;
    handLayers.reserve(kHandOverlayJoints.size() * 2);

    if (frameState.shouldRender == XR_TRUE) {
        const HmdPoseState headPose = LocateHeadPose(frameState.predictedDisplayTime);
        LocateHandJoints(frameState.predictedDisplayTime);
        UpdateTelemetry(frameState.predictedDisplayTime, headPose);
        if (!RefreshHudTexture()) {
            return false;
        }

        XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
        passthroughLayer.flags = 0;
        passthroughLayer.space = XR_NULL_HANDLE;
        passthroughLayer.layerHandle = passthroughLayer_;

        XrCompositionLayerQuad hudLayer{XR_TYPE_COMPOSITION_LAYER_QUAD};
        hudLayer.layerFlags =
            XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
            XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        hudLayer.space = viewSpace_;
        hudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        hudLayer.subImage.swapchain = hudSwapchain_;
        hudLayer.subImage.imageRect.offset = {0, 0};
        hudLayer.subImage.imageRect.extent = {
            static_cast<int32_t>(hudWidth_),
            static_cast<int32_t>(hudHeight_)};
        hudLayer.subImage.imageArrayIndex = 0;
        hudLayer.pose = IdentityPose();
        hudLayer.pose.position = {0.0f, -0.12f, -1.0f};
        hudLayer.size = {1.5f, 0.32f};

        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&passthroughLayer));
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&hudLayer));

        AppendHandLayers(leftHandOverlay_, &handLayers, &layers);
        AppendHandLayers(rightHandOverlay_, &handLayers, &layers);
    }

    XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
    frameEndInfo.displayTime = frameState.predictedDisplayTime;
    frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    frameEndInfo.layerCount = static_cast<uint32_t>(layers.size());
    frameEndInfo.layers = layers.empty() ? nullptr : layers.data();

    xrResult = xrEndFrame(session_, &frameEndInfo);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEndFrame", xrResult);
        return false;
    }

    return true;
}

bool QuestPassthroughApp::RefreshHudTexture() {
    const TelemetryState& telemetry = transport_.telemetry();
    const std::string hudSnapshot = BuildHudSnapshot(
        telemetry.connectionState,
        telemetry.packetRate,
        telemetry.trackingValid,
        telemetry.localIp,
        telemetry.targetHost);
    if (hudTextureInitialized_ && hudSnapshot == lastHudSnapshot_) {
        return true;
    }

    uint32_t imageIndex = 0;
    if (!AcquireSwapchainImageBlocking(instance_, hudSwapchain_, "hud", &imageIndex)) {
        return false;
    }
    RenderHudTexture(
        hudImages_[imageIndex].image,
        hudWidth_,
        hudHeight_,
        telemetry.connectionState,
        telemetry.packetRate,
        telemetry.trackingValid,
        telemetry.localIp,
        telemetry.targetHost);
    if (!ReleaseSwapchainImageChecked(instance_, hudSwapchain_, "hud")) {
        return false;
    }

    hudTextureInitialized_ = true;
    lastHudSnapshot_ = hudSnapshot;
    return true;
}

HmdPoseState QuestPassthroughApp::LocateHeadPose(XrTime predictedDisplayTime) {
    HmdPoseState headPose;
    XrSpaceLocation viewLocation{XR_TYPE_SPACE_LOCATION};
    const XrResult locateResult = xrLocateSpace(viewSpace_, appSpace_, predictedDisplayTime, &viewLocation);
    if (XR_SUCCEEDED(locateResult)) {
        const XrSpaceLocationFlags requiredFlags =
            XR_SPACE_LOCATION_POSITION_VALID_BIT |
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
            XR_SPACE_LOCATION_POSITION_TRACKED_BIT |
            XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
        headPose.valid = (viewLocation.locationFlags & requiredFlags) == requiredFlags;
        headPose.position = viewLocation.pose.position;
        headPose.orientation = viewLocation.pose.orientation;
    }
    return headPose;
}

void QuestPassthroughApp::LocateHandJoints(XrTime predictedDisplayTime) {
    XrHandJointsLocateInfoEXT handLocateInfo{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    handLocateInfo.baseSpace = appSpace_;
    handLocateInfo.time = predictedDisplayTime;

    const auto locateHand = [&](HandOverlayState* handState) {
        handState->tracked = false;
        for (auto& jointState : handState->joints) {
            jointState.tracked = false;
        }

        std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> jointLocations{};
        XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locations.jointCount = static_cast<uint32_t>(jointLocations.size());
        locations.jointLocations = jointLocations.data();

        const XrResult locateResult = xrLocateHandJointsEXT_(handState->tracker, &handLocateInfo, &locations);
        if (XR_FAILED(locateResult) || locations.isActive == XR_FALSE) {
            return;
        }

        handState->tracked = true;
        for (size_t jointIndex = 0; jointIndex < jointLocations.size(); ++jointIndex) {
            auto& jointState = handState->joints[jointIndex];
            const XrHandJointLocationEXT& jointLocation = jointLocations[jointIndex];
            const bool positionValid =
                (jointLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
            jointState.tracked = positionValid;
            if (!positionValid) {
                continue;
            }

            jointState.pose = jointLocation.pose;
            if ((jointLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
                jointState.pose.orientation = XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f};
            }
            jointState.radius = std::max(0.0125f, jointLocation.radius * 1.4f);
        }
    };

    locateHand(&leftHandOverlay_);
    locateHand(&rightHandOverlay_);
}

HandTelemetryState QuestPassthroughApp::BuildHandTelemetry(const HandOverlayState& handState) const {
    const auto copyPose = [](const HandJointVisualState& source) {
        TrackedPoseState pose;
        pose.tracked = source.tracked;
        pose.pose = source.pose;
        pose.radius = source.radius;
        return pose;
    };

    HandTelemetryState handTelemetry;
    handTelemetry.tracked = handState.tracked;
    handTelemetry.wrist = copyPose(handState.joints[XR_HAND_JOINT_WRIST_EXT]);
    handTelemetry.palm = copyPose(handState.joints[XR_HAND_JOINT_PALM_EXT]);
    for (size_t index = 0; index < kFingertipJoints.size(); ++index) {
        handTelemetry.fingertips[index] = copyPose(handState.joints[kFingertipJoints[index].first]);
    }
    return handTelemetry;
}

void QuestPassthroughApp::AppendHandLayers(
    const HandOverlayState& handState,
    std::vector<XrCompositionLayerQuad>* handLayers,
    std::vector<XrCompositionLayerBaseHeader*>* layers) const {
    if (!handState.tracked) {
        return;
    }

    for (const XrHandJointEXT joint : kHandOverlayJoints) {
        const auto& jointState = handState.joints[static_cast<size_t>(joint)];
        if (!jointState.tracked) {
            continue;
        }

        handLayers->push_back(XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD});
        XrCompositionLayerQuad& handLayer = handLayers->back();
        handLayer.layerFlags =
            XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
            XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
        handLayer.space = appSpace_;
        handLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        handLayer.subImage.swapchain = handOverlaySwapchain_;
        handLayer.subImage.imageRect.offset = {0, 0};
        handLayer.subImage.imageRect.extent = {
            static_cast<int32_t>(handOverlayWidth_),
            static_cast<int32_t>(handOverlayHeight_)};
        handLayer.subImage.imageArrayIndex = 0;
        handLayer.pose = jointState.pose;
        const float diameter = jointState.radius * 2.0f;
        handLayer.size = {diameter, diameter};
        layers->push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&handLayer));
    }
}

void QuestPassthroughApp::UpdateTelemetry(XrTime predictedDisplayTime, const HmdPoseState& headPose) {
    const TransportSelection selection = transport_.ReadSelection();
    transport_.telemetry().trackingValid = headPose.valid;
    transport_.SyncConnection(selection);
    transport_.UpdateStatus(selection);

    const std::vector<uint8_t> packet = transport_.SerializePacket(
        selection,
        predictedDisplayTime,
        headPose,
        BuildHandTelemetry(leftHandOverlay_),
        BuildHandTelemetry(rightHandOverlay_));
    if (transport_.SendPacket(packet)) {
        transport_.NoteSuccessfulPacketSend();
    }
    transport_.ResetPacketRateIfStale();
}

void QuestPassthroughApp::Shutdown() {
    if (!initialized_ && instance_ == XR_NULL_HANDLE && eglDisplay_ == EGL_NO_DISPLAY) {
        return;
    }
    ShutdownOpenXr();
    ShutdownEgl();
    initialized_ = false;
}

void QuestPassthroughApp::ShutdownOpenXr() {
    const auto destroyTracker = [&](XrHandTrackerEXT& tracker) {
        if (tracker != XR_NULL_HANDLE) {
            xrDestroyHandTrackerEXT_(tracker);
            tracker = XR_NULL_HANDLE;
        }
    };
    const auto destroySwapchain = [&](XrSwapchain& swapchain, std::vector<XrSwapchainImageOpenGLESKHR>& images) {
        if (swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(swapchain);
            swapchain = XR_NULL_HANDLE;
        }
        images.clear();
    };
    const auto destroySpace = [&](XrSpace& space) {
        if (space != XR_NULL_HANDLE) {
            xrDestroySpace(space);
            space = XR_NULL_HANDLE;
        }
    };

    destroyTracker(leftHandOverlay_.tracker);
    destroyTracker(rightHandOverlay_.tracker);
    destroySwapchain(handOverlaySwapchain_, handOverlayImages_);

    if (passthroughLayer_ != XR_NULL_HANDLE) {
        xrPassthroughLayerPauseFB_(passthroughLayer_);
        xrDestroyPassthroughLayerFB_(passthroughLayer_);
        passthroughLayer_ = XR_NULL_HANDLE;
    }
    if (passthrough_ != XR_NULL_HANDLE) {
        xrPassthroughPauseFB_(passthrough_);
        xrDestroyPassthroughFB_(passthrough_);
        passthrough_ = XR_NULL_HANDLE;
    }

    destroySwapchain(hudSwapchain_, hudImages_);
    destroySpace(viewSpace_);
    destroySpace(appSpace_);
    if (session_ != XR_NULL_HANDLE) {
        if (sessionRunning_) {
            xrEndSession(session_);
            sessionRunning_ = false;
        }
        xrDestroySession(session_);
        session_ = XR_NULL_HANDLE;
    }
    if (instance_ != XR_NULL_HANDLE) {
        xrDestroyInstance(instance_);
        instance_ = XR_NULL_HANDLE;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_metareader_MetaReaderActivity_nativeOnTransportModeChanged(
    JNIEnv* env,
    jclass /* clazz */,
    jstring mode,
    jint wiredPort) {
    const std::string modeText = JStringToUtf8(env, mode);
    const bool wiredMode = modeText == "WIRED";
    TransportTelemetryBridge::SetTransportModeSelection(wiredMode, static_cast<uint16_t>(wiredPort));
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_metareader_MetaReaderActivity_nativeOnServiceResolved(
    JNIEnv* env,
    jclass /* clazz */,
    jstring host,
    jint port) {
    TransportTelemetryBridge::SetWirelessEndpointSelection(JStringToUtf8(env, host), static_cast<uint16_t>(port));
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_metareader_MetaReaderActivity_nativeOnServiceLost(
    JNIEnv* /* env */,
    jclass /* clazz */) {
    TransportTelemetryBridge::ClearWirelessEndpointSelection();
}

void QuestPassthroughApp::ShutdownEgl() {
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    if (eglContext_ != EGL_NO_CONTEXT) {
        eglDestroyContext(eglDisplay_, eglContext_);
        eglContext_ = EGL_NO_CONTEXT;
    }
    if (eglSurface_ != EGL_NO_SURFACE) {
        eglDestroySurface(eglDisplay_, eglSurface_);
        eglSurface_ = EGL_NO_SURFACE;
    }
    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglTerminate(eglDisplay_);
        eglDisplay_ = EGL_NO_DISPLAY;
    }
}
