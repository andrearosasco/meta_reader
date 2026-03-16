#pragma once

#include <GLES3/gl3.h>
#include <android/log.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace quest_passthrough {

constexpr const char* kLogTag = "MetaReaderXR";

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

inline void LogError(const std::string& message) {
    __android_log_write(ANDROID_LOG_ERROR, kLogTag, message.c_str());
}

inline void LogXrResult(XrInstance instance, const char* context, XrResult result) {
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

inline XrPosef IdentityPose() {
    XrPosef pose{};
    pose.orientation.w = 1.0f;
    return pose;
}

inline constexpr std::array<XrHandJointEXT, 7> kHandOverlayJoints{
    XR_HAND_JOINT_WRIST_EXT,
    XR_HAND_JOINT_PALM_EXT,
    XR_HAND_JOINT_THUMB_TIP_EXT,
    XR_HAND_JOINT_INDEX_TIP_EXT,
    XR_HAND_JOINT_MIDDLE_TIP_EXT,
    XR_HAND_JOINT_RING_TIP_EXT,
    XR_HAND_JOINT_LITTLE_TIP_EXT,
};

inline constexpr std::array<std::pair<XrHandJointEXT, const char*>, 5> kFingertipJoints{{
    {XR_HAND_JOINT_THUMB_TIP_EXT, "thumb_tip"},
    {XR_HAND_JOINT_INDEX_TIP_EXT, "index_tip"},
    {XR_HAND_JOINT_MIDDLE_TIP_EXT, "middle_tip"},
    {XR_HAND_JOINT_RING_TIP_EXT, "ring_tip"},
    {XR_HAND_JOINT_LITTLE_TIP_EXT, "little_tip"},
}};

inline bool CreateColorSwapchain(
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

inline bool AcquireSwapchainImageBlocking(XrInstance instance, XrSwapchain swapchain, const char* label, uint32_t* imageIndex) {
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

inline bool ReleaseSwapchainImageChecked(XrInstance instance, XrSwapchain swapchain, const char* label) {
    XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    const XrResult xrResult = xrReleaseSwapchainImage(swapchain, &releaseInfo);
    if (XR_FAILED(xrResult)) {
        LogXrResult(instance, (std::string("xrReleaseSwapchainImage(") + label + ")").c_str(), xrResult);
        return false;
    }
    return true;
}

}  // namespace quest_passthrough