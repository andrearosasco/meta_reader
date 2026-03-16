#include "quest_passthrough_app.hpp"
#include "quest_passthrough_common.hpp"
#include "hud_renderer.hpp"

#include <cstdio>

using namespace quest_passthrough;

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
    if (!eglInitialize(eglDisplay_, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_ES_API)) {
        LogError("Failed to initialize EGL display or API binding.");
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

    const EGLint pbufferAttributes[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufferAttributes);
    if (eglSurface_ == EGL_NO_SURFACE) {
        LogError("eglCreatePbufferSurface failed.");
        return false;
    }

    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttributes);
    if (eglContext_ == EGL_NO_CONTEXT || !eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
        LogError("Failed to create or activate EGL context.");
        return false;
    }
    return true;
}

bool QuestPassthroughApp::InitializeOpenXr() {
    using PfnInitializeLoader = XrResult(XRAPI_PTR*)(const XrLoaderInitInfoBaseHeaderKHR*);
    PFN_xrVoidFunction loaderInitVoidFunction = nullptr;
    const XrResult loaderProcResult = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &loaderInitVoidFunction);
    if (XR_FAILED(loaderProcResult) || loaderInitVoidFunction == nullptr) {
        LogError("Failed to resolve xrInitializeLoaderKHR.");
        return false;
    }

    XrLoaderInitInfoAndroidKHR loaderInitInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInitInfo.applicationVM = app_->activity->vm;
    loaderInitInfo.applicationContext = app_->activity->clazz;
    const XrResult loaderResult = reinterpret_cast<PfnInitializeLoader>(loaderInitVoidFunction)(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInitInfo));
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
    std::snprintf(createInfo.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "%s", "MetaReader");
    std::snprintf(createInfo.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "%s", "NativeOpenXR");
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
        !GetInstanceProc("xrPassthroughLayerPauseFB", &xrPassthroughLayerPauseFB_) ||
        !GetInstanceProc("xrCreateHandTrackerEXT", &xrCreateHandTrackerEXT_) ||
        !GetInstanceProc("xrDestroyHandTrackerEXT", &xrDestroyHandTrackerEXT_) ||
        !GetInstanceProc("xrLocateHandJointsEXT", &xrLocateHandJointsEXT_)) {
        LogError("Failed to resolve required OpenXR function pointers.");
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
    if (!createSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, "local", &appSpace_) || !createSpace(XR_REFERENCE_SPACE_TYPE_VIEW, "view", &viewSpace_)) {
        return false;
    }

    XrPassthroughCreateInfoFB passthroughCreateInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    xrResult = xrCreatePassthroughFB_(session_, &passthroughCreateInfo, &passthrough_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrCreatePassthroughFB", xrResult);
        return false;
    }

    XrPassthroughLayerCreateInfoFB passthroughLayerCreateInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    passthroughLayerCreateInfo.passthrough = passthrough_;
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
    return ReleaseSwapchainImageChecked(instance_, handOverlaySwapchain_, "hand init");
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
        if (eventBuffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            HandleSessionStateChanged(*reinterpret_cast<const XrEventDataSessionStateChanged*>(&eventBuffer), shouldExit);
        } else if (eventBuffer.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            *shouldExit = true;
        }
        eventBuffer = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void QuestPassthroughApp::HandleSessionStateChanged(const XrEventDataSessionStateChanged& stateChangedEvent, bool* shouldExit) {
    switch (stateChangedEvent.state) {
        case XR_SESSION_STATE_READY: {
            if (sessionRunning_) {
                break;
            }
            XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            const XrResult xrResult = xrBeginSession(session_, &beginInfo);
            if (XR_FAILED(xrResult)) {
                LogXrResult(instance_, "xrBeginSession", xrResult);
                *shouldExit = true;
                return;
            }
            sessionRunning_ = true;
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