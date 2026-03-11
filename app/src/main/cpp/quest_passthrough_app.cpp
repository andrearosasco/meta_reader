#include "quest_passthrough_app.hpp"

#include <jni.h>
#include <android/log.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <ifaddrs.h>
#include <net/if.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace {

constexpr const char* kLogTag = "MetaReaderXR";

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

void LogInfo(const std::string& message) {
    __android_log_write(ANDROID_LOG_INFO, kLogTag, message.c_str());
}

void LogError(const std::string& message) {
    __android_log_write(ANDROID_LOG_ERROR, kLogTag, message.c_str());
}

struct DiscoveryTelemetryState {
    std::string connectionState{"DISCOVERING"};
    std::string targetHost{"SEARCHING"};
    std::string serviceName;
};

std::mutex gDiscoveryTelemetryMutex;
DiscoveryTelemetryState gDiscoveryTelemetryState;

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

void SetDiscoveryTelemetryState(
    const std::string& connectionState,
    const std::string& targetHost,
    const std::string& serviceName) {
    std::scoped_lock lock(gDiscoveryTelemetryMutex);
    gDiscoveryTelemetryState.connectionState = connectionState;
    gDiscoveryTelemetryState.targetHost = targetHost;
    gDiscoveryTelemetryState.serviceName = serviceName;
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

XrQuaternionf NormalizeQuaternion(XrQuaternionf quaternion) {
    const float length = std::sqrt(
        quaternion.x * quaternion.x +
        quaternion.y * quaternion.y +
        quaternion.z * quaternion.z +
        quaternion.w * quaternion.w);
    if (length <= 0.0f) {
        return XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f};
    }

    const float inverseLength = 1.0f / length;
    quaternion.x *= inverseLength;
    quaternion.y *= inverseLength;
    quaternion.z *= inverseLength;
    quaternion.w *= inverseLength;
    return quaternion;
}

XrQuaternionf MultiplyQuaternion(const XrQuaternionf& left, const XrQuaternionf& right) {
    return NormalizeQuaternion(XrQuaternionf{
        left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z,
    });
}

const std::array<XrHandJointEXT, 7>& HandOverlayJointSet() {
    static const std::array<XrHandJointEXT, 7> joints{
        XR_HAND_JOINT_WRIST_EXT,
        XR_HAND_JOINT_PALM_EXT,
        XR_HAND_JOINT_THUMB_TIP_EXT,
        XR_HAND_JOINT_INDEX_TIP_EXT,
        XR_HAND_JOINT_MIDDLE_TIP_EXT,
        XR_HAND_JOINT_RING_TIP_EXT,
        XR_HAND_JOINT_LITTLE_TIP_EXT,
    };
    return joints;
}

const std::array<uint8_t, 7>& GlyphFor(char character) {
    static const std::array<uint8_t, 7> blank{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const std::array<uint8_t, 7> dash{0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00};
    static const std::array<uint8_t, 7> dot{0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c};
    static const std::array<uint8_t, 7> colon{0x00, 0x0c, 0x0c, 0x00, 0x0c, 0x0c, 0x00};

    static const std::array<uint8_t, 7> zero{0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e};
    static const std::array<uint8_t, 7> one{0x04, 0x0c, 0x14, 0x04, 0x04, 0x04, 0x1f};
    static const std::array<uint8_t, 7> two{0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f};
    static const std::array<uint8_t, 7> three{0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e};
    static const std::array<uint8_t, 7> four{0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02};
    static const std::array<uint8_t, 7> five{0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e};
    static const std::array<uint8_t, 7> six{0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e};
    static const std::array<uint8_t, 7> seven{0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    static const std::array<uint8_t, 7> eight{0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e};
    static const std::array<uint8_t, 7> nine{0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e};

    static const std::array<uint8_t, 7> a{0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const std::array<uint8_t, 7> b{0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e};
    static const std::array<uint8_t, 7> c{0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e};
    static const std::array<uint8_t, 7> d{0x1c, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1c};
    static const std::array<uint8_t, 7> e{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
    static const std::array<uint8_t, 7> f{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10};
    static const std::array<uint8_t, 7> g{0x0f, 0x10, 0x10, 0x17, 0x11, 0x11, 0x0f};
    static const std::array<uint8_t, 7> h{0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const std::array<uint8_t, 7> i{0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f};
    static const std::array<uint8_t, 7> j{0x1f, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0c};
    static const std::array<uint8_t, 7> k{0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    static const std::array<uint8_t, 7> l{0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
    static const std::array<uint8_t, 7> m{0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11};
    static const std::array<uint8_t, 7> n{0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11};
    static const std::array<uint8_t, 7> o{0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const std::array<uint8_t, 7> p{0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10};
    static const std::array<uint8_t, 7> q{0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d};
    static const std::array<uint8_t, 7> r{0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
    static const std::array<uint8_t, 7> s{0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e};
    static const std::array<uint8_t, 7> t{0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const std::array<uint8_t, 7> u{0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
    static const std::array<uint8_t, 7> v{0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04};
    static const std::array<uint8_t, 7> w{0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a};
    static const std::array<uint8_t, 7> x{0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11};
    static const std::array<uint8_t, 7> y{0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04};
    static const std::array<uint8_t, 7> z{0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f};

    switch (character) {
        case '0': return zero;
        case '1': return one;
        case '2': return two;
        case '3': return three;
        case '4': return four;
        case '5': return five;
        case '6': return six;
        case '7': return seven;
        case '8': return eight;
        case '9': return nine;
        case 'A': return a;
        case 'B': return b;
        case 'C': return c;
        case 'D': return d;
        case 'E': return e;
        case 'F': return f;
        case 'G': return g;
        case 'H': return h;
        case 'I': return i;
        case 'J': return j;
        case 'K': return k;
        case 'L': return l;
        case 'M': return m;
        case 'N': return n;
        case 'O': return o;
        case 'P': return p;
        case 'Q': return q;
        case 'R': return r;
        case 'S': return s;
        case 'T': return t;
        case 'U': return u;
        case 'V': return v;
        case 'W': return w;
        case 'X': return x;
        case 'Y': return y;
        case 'Z': return z;
        case '-': return dash;
        case '.': return dot;
        case ':': return colon;
        case ' ': return blank;
        default: return blank;
    }
}

void BlendPixel(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, int x, int y,
                uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
        return;
    }

    const size_t index = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4;
    const float srcAlpha = static_cast<float>(alpha) / 255.0f;
    const float dstAlpha = static_cast<float>(pixels[index + 3]) / 255.0f;
    const float outAlpha = srcAlpha + dstAlpha * (1.0f - srcAlpha);
    if (outAlpha <= 0.0f) {
        return;
    }

    auto blendChannel = [&](uint8_t src, size_t offset) {
        const float dst = static_cast<float>(pixels[index + offset]);
        const float out = (static_cast<float>(src) * srcAlpha + dst * dstAlpha * (1.0f - srcAlpha)) / outAlpha;
        pixels[index + offset] = static_cast<uint8_t>(std::clamp(out, 0.0f, 255.0f));
    };

    blendChannel(red, 0);
    blendChannel(green, 1);
    blendChannel(blue, 2);
    pixels[index + 3] = static_cast<uint8_t>(std::clamp(outAlpha * 255.0f, 0.0f, 255.0f));
}

void DrawFilledRect(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
                    int x, int y, int rectWidth, int rectHeight,
                    uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    for (int row = 0; row < rectHeight; ++row) {
        for (int column = 0; column < rectWidth; ++column) {
            BlendPixel(pixels, width, height, x + column, y + row, red, green, blue, alpha);
        }
    }
}

void DrawChar(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
              int x, int y, int scale, char character,
              uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    const auto& glyph = GlyphFor(character);
    for (int row = 0; row < 7; ++row) {
        for (int column = 0; column < 5; ++column) {
            if ((glyph[row] & (1 << (4 - column))) == 0) {
                continue;
            }
            for (int scaleY = 0; scaleY < scale; ++scaleY) {
                for (int scaleX = 0; scaleX < scale; ++scaleX) {
                    BlendPixel(
                        pixels, width, height,
                        x + column * scale + scaleX,
                        y + row * scale + scaleY,
                        red, green, blue, alpha);
                }
            }
        }
    }
}

void DrawText(std::vector<uint8_t>& pixels, uint32_t width, uint32_t height,
              int x, int y, int scale, const std::string& text,
              uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    int cursorX = x;
    for (char character : text) {
        DrawChar(pixels, width, height, cursorX, y, scale, character, red, green, blue, alpha);
        cursorX += 6 * scale;
    }
}

}  // namespace

QuestPassthroughApp::QuestPassthroughApp(android_app* app) : app_(app) {}

QuestPassthroughApp::~QuestPassthroughApp() {
    Shutdown();
}

void QuestPassthroughApp::HandleAppCommand(int32_t cmd) {
    switch (cmd) {
        case APP_CMD_RESUME:
            resumed_ = true;
            break;
        case APP_CMD_PAUSE:
            resumed_ = false;
            break;
        case APP_CMD_DESTROY:
            exitRequested_ = true;
            break;
        default:
            break;
    }
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
    LogInfo("Quest passthrough app initialized.");
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
    PfnInitializeLoader xrInitializeLoaderKHRFn = nullptr;

    PFN_xrVoidFunction loaderInitVoidFunction = nullptr;
    XrResult loaderProcResult = xrGetInstanceProcAddr(
        XR_NULL_HANDLE,
        "xrInitializeLoaderKHR",
        &loaderInitVoidFunction);
    if (XR_SUCCEEDED(loaderProcResult) && loaderInitVoidFunction != nullptr) {
        xrInitializeLoaderKHRFn = reinterpret_cast<PfnInitializeLoader>(loaderInitVoidFunction);
    }

    void* loaderLibrary = nullptr;
    if (xrInitializeLoaderKHRFn == nullptr) {
        loaderLibrary = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
        if (loaderLibrary != nullptr) {
            xrInitializeLoaderKHRFn = reinterpret_cast<PfnInitializeLoader>(
                dlsym(loaderLibrary, "xrInitializeLoaderKHR"));
        }
    }

    if (xrInitializeLoaderKHRFn == nullptr) {
        LogError("Failed to resolve xrInitializeLoaderKHR via xrGetInstanceProcAddr or libopenxr_loader.so.");
        if (loaderLibrary != nullptr) {
            dlclose(loaderLibrary);
        }
        return false;
    }

    XrLoaderInitInfoAndroidKHR loaderInitInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInitInfo.applicationVM = app_->activity->vm;
    loaderInitInfo.applicationContext = app_->activity->clazz;

    const XrResult loaderResult = xrInitializeLoaderKHRFn(
        reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInitInfo));
    if (loaderLibrary != nullptr) {
        dlclose(loaderLibrary);
    }
    if (XR_FAILED(loaderResult)) {
        LogXrResult(XR_NULL_HANDLE, "xrInitializeLoaderKHR", loaderResult);
        return false;
    }

    uint32_t extensionCount = 0;
    XrResult xrResult = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
    if (XR_FAILED(xrResult)) {
        LogXrResult(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties(count)", xrResult);
        return false;
    }

    std::vector<XrExtensionProperties> extensionProperties(
        extensionCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    xrResult = xrEnumerateInstanceExtensionProperties(
        nullptr, extensionCount, &extensionCount, extensionProperties.data());
    if (XR_FAILED(xrResult)) {
        LogXrResult(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties(list)", xrResult);
        return false;
    }

    availableInstanceExtensions_.clear();
    availableInstanceExtensions_.reserve(extensionProperties.size());
    for (const auto& extension : extensionProperties) {
        availableInstanceExtensions_.emplace_back(extension.extensionName);
    }

    const std::array<const char*, 3> requiredExtensions{
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_FB_PASSTHROUGH_EXTENSION_NAME,
    };

    std::vector<const char*> enabledExtensions(requiredExtensions.begin(), requiredExtensions.end());
    if (IsInstanceExtensionSupported(XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
        enabledExtensions.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        handTrackingSupported_ = true;
    } else {
        handTrackingSupported_ = false;
        LogInfo("XR_EXT_hand_tracking is unavailable; hand overlays disabled.");
    }

    for (const char* requiredExtension : requiredExtensions) {
        if (!IsInstanceExtensionSupported(requiredExtension)) {
            std::ostringstream stream;
            stream << "Missing required instance extension: " << requiredExtension;
            LogError(stream.str());
            return false;
        }
    }

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

    xrResult = xrCreateInstance(&createInfo, &instance_);
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

    if (handTrackingSupported_) {
        if (!GetInstanceProc("xrCreateHandTrackerEXT", &xrCreateHandTrackerEXT_) ||
            !GetInstanceProc("xrDestroyHandTrackerEXT", &xrDestroyHandTrackerEXT_) ||
            !GetInstanceProc("xrLocateHandJointsEXT", &xrLocateHandJointsEXT_)) {
            LogError("Failed to resolve XR_EXT_hand_tracking function pointers.");
            return false;
        }
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

    if (!ChooseEnvironmentBlendMode()) {
        return false;
    }

    if (!InitializeSpaces() || !InitializePassthrough() || !InitializeHudSwapchain()) {
        return false;
    }

    if (handTrackingSupported_) {
        if (!InitializeHandTracking() || !InitializeHandOverlaySwapchain()) {
            return false;
        }
    }

    telemetry_.localIp = DetectLocalIp();
    return true;
}

bool QuestPassthroughApp::InitializeSpaces() {
    XrReferenceSpaceCreateInfo appSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    appSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    appSpaceCreateInfo.poseInReferenceSpace = IdentityPose();

    XrResult xrResult = xrCreateReferenceSpace(session_, &appSpaceCreateInfo, &appSpace_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrCreateReferenceSpace(local)", xrResult);
        return false;
    }

    XrReferenceSpaceCreateInfo viewSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceCreateInfo.poseInReferenceSpace = IdentityPose();

    xrResult = xrCreateReferenceSpace(session_, &viewSpaceCreateInfo, &viewSpace_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrCreateReferenceSpace(view)", xrResult);
        return false;
    }

    return true;
}

bool QuestPassthroughApp::InitializePassthrough() {
    XrPassthroughCreateInfoFB passthroughCreateInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    passthroughCreateInfo.flags = 0;

    XrResult xrResult = xrCreatePassthroughFB_(session_, &passthroughCreateInfo, &passthrough_);
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

    return true;
}

bool QuestPassthroughApp::InitializeHudSwapchain() {
    uint32_t formatCount = 0;
    XrResult xrResult = xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEnumerateSwapchainFormats(count)", xrResult);
        return false;
    }

    std::vector<int64_t> formats(formatCount);
    xrResult = xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data());
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEnumerateSwapchainFormats(list)", xrResult);
        return false;
    }

    const std::array<int64_t, 2> preferredFormats{GL_SRGB8_ALPHA8, GL_RGBA8};
    auto preferredIt = std::find_first_of(
        formats.begin(), formats.end(), preferredFormats.begin(), preferredFormats.end());
    hudSwapchainFormat_ = preferredIt != formats.end() ? *preferredIt : formats.front();

    XrSwapchainCreateInfo swapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainCreateInfo.format = hudSwapchainFormat_;
    swapchainCreateInfo.sampleCount = 1;
    swapchainCreateInfo.width = hudWidth_;
    swapchainCreateInfo.height = hudHeight_;
    swapchainCreateInfo.faceCount = 1;
    swapchainCreateInfo.arraySize = 1;
    swapchainCreateInfo.mipCount = 1;
    swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

    xrResult = xrCreateSwapchain(session_, &swapchainCreateInfo, &hudSwapchain_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrCreateSwapchain(hud)", xrResult);
        return false;
    }

    uint32_t imageCount = 0;
    xrResult = xrEnumerateSwapchainImages(hudSwapchain_, 0, &imageCount, nullptr);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEnumerateSwapchainImages(count)", xrResult);
        return false;
    }

    hudImages_.assign(imageCount, XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrResult = xrEnumerateSwapchainImages(
        hudSwapchain_,
        imageCount,
        &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(hudImages_.data()));
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEnumerateSwapchainImages(list)", xrResult);
        return false;
    }

    return true;
}

bool QuestPassthroughApp::InitializeHandTracking() {
    XrHandTrackerCreateInfoEXT createInfo{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
    createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;

    createInfo.hand = XR_HAND_LEFT_EXT;
    XrResult xrResult = xrCreateHandTrackerEXT_(session_, &createInfo, &leftHandOverlay_.tracker);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrCreateHandTrackerEXT(left)", xrResult);
        return false;
    }

    createInfo.hand = XR_HAND_RIGHT_EXT;
    xrResult = xrCreateHandTrackerEXT_(session_, &createInfo, &rightHandOverlay_.tracker);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrCreateHandTrackerEXT(right)", xrResult);
        return false;
    }

    return true;
}

bool QuestPassthroughApp::InitializeHandOverlaySwapchain() {
    uint32_t formatCount = 0;
    XrResult xrResult = xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEnumerateSwapchainFormats(hand count)", xrResult);
        return false;
    }

    std::vector<int64_t> formats(formatCount);
    xrResult = xrEnumerateSwapchainFormats(session_, formatCount, &formatCount, formats.data());
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEnumerateSwapchainFormats(hand list)", xrResult);
        return false;
    }

    const std::array<int64_t, 2> preferredFormats{GL_SRGB8_ALPHA8, GL_RGBA8};
    auto preferredIt = std::find_first_of(
        formats.begin(), formats.end(), preferredFormats.begin(), preferredFormats.end());
    const int64_t handFormat = preferredIt != formats.end() ? *preferredIt : formats.front();

    XrSwapchainCreateInfo swapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainCreateInfo.format = handFormat;
    swapchainCreateInfo.sampleCount = 1;
    swapchainCreateInfo.width = handOverlayWidth_;
    swapchainCreateInfo.height = handOverlayHeight_;
    swapchainCreateInfo.faceCount = 1;
    swapchainCreateInfo.arraySize = 1;
    swapchainCreateInfo.mipCount = 1;
    swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

    xrResult = xrCreateSwapchain(session_, &swapchainCreateInfo, &handOverlaySwapchain_);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrCreateSwapchain(hand overlay)", xrResult);
        return false;
    }

    uint32_t imageCount = 0;
    xrResult = xrEnumerateSwapchainImages(handOverlaySwapchain_, 0, &imageCount, nullptr);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEnumerateSwapchainImages(hand count)", xrResult);
        return false;
    }

    handOverlayImages_.assign(imageCount, XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrResult = xrEnumerateSwapchainImages(
        handOverlaySwapchain_,
        imageCount,
        &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(handOverlayImages_.data()));
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEnumerateSwapchainImages(hand list)", xrResult);
        return false;
    }

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrResult = xrAcquireSwapchainImage(handOverlaySwapchain_, &acquireInfo, &imageIndex);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrAcquireSwapchainImage(hand init)", xrResult);
        return false;
    }

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    xrResult = xrWaitSwapchainImage(handOverlaySwapchain_, &waitInfo);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrWaitSwapchainImage(hand init)", xrResult);
        return false;
    }

    RenderHandOverlayToSwapchain(imageIndex);

    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrResult = xrReleaseSwapchainImage(handOverlaySwapchain_, &releaseInfo);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrReleaseSwapchainImage(hand init)", xrResult);
        return false;
    }

    handOverlayTextureInitialized_ = true;

    return true;
}

bool QuestPassthroughApp::ChooseEnvironmentBlendMode() {
    uint32_t blendModeCount = 0;
    XrResult xrResult = xrEnumerateEnvironmentBlendModes(
        instance_,
        systemId_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &blendModeCount,
        nullptr);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEnumerateEnvironmentBlendModes(count)", xrResult);
        return false;
    }

    std::vector<XrEnvironmentBlendMode> blendModes(blendModeCount);
    xrResult = xrEnumerateEnvironmentBlendModes(
        instance_,
        systemId_,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        blendModeCount,
        &blendModeCount,
        blendModes.data());
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEnumerateEnvironmentBlendModes(list)", xrResult);
        return false;
    }

    const auto alphaBlendIt = std::find(
        blendModes.begin(),
        blendModes.end(),
        XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND);
    if (alphaBlendIt != blendModes.end()) {
        environmentBlendMode_ = *alphaBlendIt;
        return true;
    }

    const auto additiveIt = std::find(
        blendModes.begin(),
        blendModes.end(),
        XR_ENVIRONMENT_BLEND_MODE_ADDITIVE);
    if (additiveIt != blendModes.end()) {
        environmentBlendMode_ = *additiveIt;
        return true;
    }

    const auto opaqueIt = std::find(blendModes.begin(), blendModes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE);
    environmentBlendMode_ = opaqueIt != blendModes.end() ? *opaqueIt : blendModes.front();
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
    sessionState_ = stateChangedEvent.state;

    switch (sessionState_) {
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
                const XrResult xrResult = xrEndSession(session_);
                if (XR_FAILED(xrResult)) {
                    LogXrResult(instance_, "xrEndSession", xrResult);
                }
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
    handLayers.reserve(HandOverlayJointSet().size() * 2);

    if (frameState.shouldRender == XR_TRUE) {
        UpdateTelemetry(frameState.predictedDisplayTime);

        const std::string hudSnapshot = MakeHudSnapshot();
        if (!hudTextureInitialized_ || hudSnapshot != lastHudSnapshot_) {
            uint32_t imageIndex = 0;
            XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            xrResult = xrAcquireSwapchainImage(hudSwapchain_, &acquireInfo, &imageIndex);
            if (XR_FAILED(xrResult)) {
                LogXrResult(instance_, "xrAcquireSwapchainImage", xrResult);
                return false;
            }

            XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = XR_INFINITE_DURATION;
            xrResult = xrWaitSwapchainImage(hudSwapchain_, &waitInfo);
            if (XR_FAILED(xrResult)) {
                LogXrResult(instance_, "xrWaitSwapchainImage", xrResult);
                return false;
            }

            RenderHudToSwapchain(imageIndex);

            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrResult = xrReleaseSwapchainImage(hudSwapchain_, &releaseInfo);
            if (XR_FAILED(xrResult)) {
                LogXrResult(instance_, "xrReleaseSwapchainImage", xrResult);
                return false;
            }

            hudTextureInitialized_ = true;
            lastHudSnapshot_ = hudSnapshot;
        }

        if (handTrackingSupported_ && handOverlaySwapchain_ != XR_NULL_HANDLE) {
            XrHandJointsLocateInfoEXT handLocateInfo{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
            handLocateInfo.baseSpace = appSpace_;
            handLocateInfo.time = frameState.predictedDisplayTime;

            const auto locateHand = [&](HandOverlayState* handState) {
                handState->tracked = false;
                if (handState->tracker == XR_NULL_HANDLE) {
                    return;
                }

                std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> jointLocations{
                    XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{},
                    XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{},
                    XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{},
                    XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{},
                    XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{},
                    XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{}, XrHandJointLocationEXT{},
                    XrHandJointLocationEXT{}, XrHandJointLocationEXT{}
                };
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
                    const XrSpaceLocationFlags requiredFlags =
                        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
                    jointState.tracked = (jointLocation.locationFlags & requiredFlags) == requiredFlags;
                    if (!jointState.tracked) {
                        continue;
                    }

                    jointState.pose = jointLocation.pose;
                    jointState.radius = std::max(0.0125f, jointLocation.radius * 1.4f);
                }
            };

            locateHand(&leftHandOverlay_);
            locateHand(&rightHandOverlay_);
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

        auto appendHandLayers = [&](const HandOverlayState& handState) {
            if (!handOverlayTextureInitialized_ || !handState.tracked) {
                return;
            }

            for (const XrHandJointEXT joint : HandOverlayJointSet()) {
                const auto& jointState = handState.joints[static_cast<size_t>(joint)];
                if (!jointState.tracked) {
                    continue;
                }

                handLayers.push_back(XrCompositionLayerQuad{XR_TYPE_COMPOSITION_LAYER_QUAD});
                XrCompositionLayerQuad& handLayer = handLayers.back();
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
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&handLayer));
            }
        };

        appendHandLayers(leftHandOverlay_);
        appendHandLayers(rightHandOverlay_);
    }

    XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
    frameEndInfo.displayTime = frameState.predictedDisplayTime;
    frameEndInfo.environmentBlendMode = environmentBlendMode_;
    frameEndInfo.layerCount = static_cast<uint32_t>(layers.size());
    frameEndInfo.layers = layers.empty() ? nullptr : layers.data();

    xrResult = xrEndFrame(session_, &frameEndInfo);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance_, "xrEndFrame", xrResult);
        return false;
    }

    return true;
}

void QuestPassthroughApp::UpdateTelemetry(XrTime predictedDisplayTime) {
    UpdateDiscoveryTelemetry();

    if (telemetry_.localIp == "LOOKUP") {
        telemetry_.localIp = DetectLocalIp();
    }

    telemetry_.trackingValid = false;

    std::array<XrView, 2> views{{XrView{XR_TYPE_VIEW}, XrView{XR_TYPE_VIEW}}};
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = predictedDisplayTime;
    locateInfo.space = appSpace_;

    uint32_t viewCountOutput = 0;
    const XrResult xrResult = xrLocateViews(
        session_,
        &locateInfo,
        &viewState,
        static_cast<uint32_t>(views.size()),
        &viewCountOutput,
        views.data());
    if (XR_SUCCEEDED(xrResult) && viewCountOutput == views.size()) {
        const XrViewStateFlags requiredFlags =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT |
            XR_VIEW_STATE_POSITION_VALID_BIT |
            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
            XR_VIEW_STATE_POSITION_TRACKED_BIT;
        telemetry_.trackingValid = (viewState.viewStateFlags & requiredFlags) == requiredFlags;
    }
}

void QuestPassthroughApp::UpdateDiscoveryTelemetry() {
    std::scoped_lock lock(gDiscoveryTelemetryMutex);
    telemetry_.connectionState = gDiscoveryTelemetryState.connectionState;
    telemetry_.targetHost = gDiscoveryTelemetryState.targetHost;
}

void QuestPassthroughApp::RenderHudToSwapchain(uint32_t imageIndex) {
    std::vector<uint8_t> pixels(static_cast<size_t>(hudWidth_) * hudHeight_ * 4, 0);

    const int panelX = 16;
    const int panelY = 16;
    const int panelWidth = static_cast<int>(hudWidth_) - 32;
    const int panelHeight = static_cast<int>(hudHeight_) - 32;
    const int titleScale = 4;
    const int contentScale = 3;
    const int leftLabelX = 40;
    const int leftValueX = 280;
    const int rightLabelX = 760;
    const int rightValueX = 1020;

    DrawFilledRect(pixels, hudWidth_, hudHeight_, panelX, panelY, panelWidth, panelHeight, 8, 10, 18, 196);
    DrawFilledRect(pixels, hudWidth_, hudHeight_, panelX, panelY, panelWidth, 4, 0, 176, 255, 220);

    DrawText(pixels, hudWidth_, hudHeight_, 40, 34, titleScale, "META READER", 255, 255, 255, 255);

    std::ostringstream packetRateStream;
    packetRateStream << std::fixed << std::setprecision(1) << telemetry_.packetRate << " PPS";

    const std::string trackingText = telemetry_.trackingValid ? "VALID" : "INVALID";

    DrawText(pixels, hudWidth_, hudHeight_, leftLabelX, 90, contentScale, "CONNECTION:", 130, 180, 255, 255);
    DrawText(
        pixels,
        hudWidth_,
        hudHeight_,
        leftValueX,
        90,
        contentScale,
        telemetry_.connectionState,
        telemetry_.connectionState == "CONNECTED" ? 64 : 255,
        telemetry_.connectionState == "CONNECTED" ? 255 : 88,
        telemetry_.connectionState == "CONNECTED" ? 128 : 88,
        255);

    DrawText(pixels, hudWidth_, hudHeight_, leftLabelX, 126, contentScale, "PACKET RATE:", 130, 180, 255, 255);
    DrawText(pixels, hudWidth_, hudHeight_, leftValueX, 126, contentScale, packetRateStream.str(), 255, 255, 255, 255);

    DrawText(pixels, hudWidth_, hudHeight_, leftLabelX, 162, contentScale, "TRACKING:", 130, 180, 255, 255);
    DrawText(
        pixels,
        hudWidth_,
        hudHeight_,
        leftValueX,
        162,
        contentScale,
        trackingText,
        telemetry_.trackingValid ? 64 : 255,
        telemetry_.trackingValid ? 255 : 88,
        telemetry_.trackingValid ? 128 : 88,
        255);

    DrawText(pixels, hudWidth_, hudHeight_, rightLabelX, 90, contentScale, "LOCAL IP:", 130, 180, 255, 255);
    DrawText(pixels, hudWidth_, hudHeight_, rightValueX, 90, contentScale, telemetry_.localIp, 255, 255, 255, 255);

    DrawText(pixels, hudWidth_, hudHeight_, rightLabelX, 126, contentScale, "TARGET HOST:", 130, 180, 255, 255);
    DrawText(pixels, hudWidth_, hudHeight_, rightValueX, 126, contentScale, telemetry_.targetHost, 255, 255, 255, 255);

    std::vector<uint8_t> uploadPixels(pixels.size());
    const size_t rowBytes = static_cast<size_t>(hudWidth_) * 4;
    for (uint32_t row = 0; row < hudHeight_; ++row) {
        const size_t srcOffset = static_cast<size_t>(row) * rowBytes;
        const size_t dstOffset = static_cast<size_t>(hudHeight_ - 1 - row) * rowBytes;
        std::memcpy(uploadPixels.data() + dstOffset, pixels.data() + srcOffset, rowBytes);
    }

    glBindTexture(GL_TEXTURE_2D, hudImages_[imageIndex].image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        static_cast<GLsizei>(hudWidth_),
        static_cast<GLsizei>(hudHeight_),
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        uploadPixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();
}

void QuestPassthroughApp::RenderHandOverlayToSwapchain(uint32_t imageIndex) {
    std::vector<uint8_t> pixels(static_cast<size_t>(handOverlayWidth_) * handOverlayHeight_ * 4, 0);
    const float centerX = static_cast<float>(handOverlayWidth_) * 0.5f;
    const float centerY = static_cast<float>(handOverlayHeight_) * 0.5f;
    const float outerRadius = static_cast<float>(handOverlayWidth_) * 0.42f;
    const float innerRadius = static_cast<float>(handOverlayWidth_) * 0.24f;

    for (uint32_t y = 0; y < handOverlayHeight_; ++y) {
        for (uint32_t x = 0; x < handOverlayWidth_; ++x) {
            const float dx = static_cast<float>(x) - centerX;
            const float dy = static_cast<float>(y) - centerY;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance > outerRadius) {
                continue;
            }

            const size_t index = (static_cast<size_t>(y) * handOverlayWidth_ + static_cast<size_t>(x)) * 4;
            if (distance >= innerRadius) {
                pixels[index + 0] = 0;
                pixels[index + 1] = 220;
                pixels[index + 2] = 255;
                pixels[index + 3] = 224;
            } else {
                pixels[index + 0] = 0;
                pixels[index + 1] = 120;
                pixels[index + 2] = 255;
                pixels[index + 3] = 72;
            }
        }
    }

    glBindTexture(GL_TEXTURE_2D, handOverlayImages_[imageIndex].image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        static_cast<GLsizei>(handOverlayWidth_),
        static_cast<GLsizei>(handOverlayHeight_),
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    glFlush();
}

std::string QuestPassthroughApp::MakeHudSnapshot() const {
    std::ostringstream stream;
    stream << telemetry_.connectionState << '|'
           << std::fixed << std::setprecision(1) << telemetry_.packetRate << '|'
           << (telemetry_.trackingValid ? '1' : '0') << '|'
           << telemetry_.localIp << '|'
           << telemetry_.targetHost;
    return stream.str();
}

std::string QuestPassthroughApp::DetectLocalIp() const {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0 || interfaces == nullptr) {
        return "UNAVAILABLE";
    }

    std::string result = "UNAVAILABLE";
    for (ifaddrs* interface = interfaces; interface != nullptr; interface = interface->ifa_next) {
        if (interface->ifa_addr == nullptr || interface->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if ((interface->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        char buffer[INET_ADDRSTRLEN]{};
        const auto* address = reinterpret_cast<const sockaddr_in*>(interface->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, buffer, sizeof(buffer)) != nullptr) {
            result = buffer;
            break;
        }
    }

    freeifaddrs(interfaces);
    return result;
}

bool QuestPassthroughApp::IsInstanceExtensionSupported(const char* extensionName) const {
    return std::any_of(
        availableInstanceExtensions_.begin(),
        availableInstanceExtensions_.end(),
        [&](const std::string& availableExtension) {
            return availableExtension == extensionName;
        });
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
    if (leftHandOverlay_.tracker != XR_NULL_HANDLE && xrDestroyHandTrackerEXT_ != nullptr) {
        xrDestroyHandTrackerEXT_(leftHandOverlay_.tracker);
        leftHandOverlay_.tracker = XR_NULL_HANDLE;
    }
    if (rightHandOverlay_.tracker != XR_NULL_HANDLE && xrDestroyHandTrackerEXT_ != nullptr) {
        xrDestroyHandTrackerEXT_(rightHandOverlay_.tracker);
        rightHandOverlay_.tracker = XR_NULL_HANDLE;
    }

    if (handOverlaySwapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(handOverlaySwapchain_);
        handOverlaySwapchain_ = XR_NULL_HANDLE;
    }
    handOverlayImages_.clear();

    if (passthroughLayer_ != XR_NULL_HANDLE) {
        if (xrPassthroughLayerPauseFB_ != nullptr) {
            xrPassthroughLayerPauseFB_(passthroughLayer_);
        }
        if (xrDestroyPassthroughLayerFB_ != nullptr) {
            xrDestroyPassthroughLayerFB_(passthroughLayer_);
        }
        passthroughLayer_ = XR_NULL_HANDLE;
    }

    if (passthrough_ != XR_NULL_HANDLE) {
        if (xrPassthroughPauseFB_ != nullptr) {
            xrPassthroughPauseFB_(passthrough_);
        }
        if (xrDestroyPassthroughFB_ != nullptr) {
            xrDestroyPassthroughFB_(passthrough_);
        }
        passthrough_ = XR_NULL_HANDLE;
    }

    if (hudSwapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(hudSwapchain_);
        hudSwapchain_ = XR_NULL_HANDLE;
    }
    hudImages_.clear();

    if (viewSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(viewSpace_);
        viewSpace_ = XR_NULL_HANDLE;
    }
    if (appSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(appSpace_);
        appSpace_ = XR_NULL_HANDLE;
    }

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
Java_com_example_metareader_MetaReaderActivity_nativeOnDiscoveryState(
    JNIEnv* env,
    jclass /* clazz */,
    jstring state,
    jstring detail) {
    const std::string stateText = JStringToUtf8(env, state);
    const std::string detailText = JStringToUtf8(env, detail);
    SetDiscoveryTelemetryState(stateText.empty() ? "DISCOVERING" : stateText, "SEARCHING", {});

    std::ostringstream stream;
    stream << "Service discovery state: " << (stateText.empty() ? "DISCOVERING" : stateText);
    if (!detailText.empty()) {
        stream << " (" << detailText << ")";
    }
    LogInfo(stream.str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_metareader_MetaReaderActivity_nativeOnServiceResolved(
    JNIEnv* env,
    jclass /* clazz */,
    jstring serviceName,
    jstring host,
    jint port,
    jstring txtSummary) {
    const std::string serviceNameText = JStringToUtf8(env, serviceName);
    const std::string hostText = JStringToUtf8(env, host);
    const std::string txtSummaryText = JStringToUtf8(env, txtSummary);

    std::ostringstream endpointStream;
    endpointStream << hostText;
    if (port > 0) {
        endpointStream << ':' << port;
    }

    SetDiscoveryTelemetryState("DISCOVERED", endpointStream.str(), serviceNameText);

    std::ostringstream stream;
    stream << "Resolved service " << serviceNameText << " -> " << endpointStream.str();
    if (!txtSummaryText.empty()) {
        stream << " [" << txtSummaryText << ']';
    }
    LogInfo(stream.str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_metareader_MetaReaderActivity_nativeOnServiceLost(
    JNIEnv* env,
    jclass /* clazz */,
    jstring serviceName) {
    const std::string serviceNameText = JStringToUtf8(env, serviceName);
    SetDiscoveryTelemetryState("SEARCHING", "SEARCHING", serviceNameText);

    std::ostringstream stream;
    stream << "Lost service " << serviceNameText;
    LogInfo(stream.str());
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
