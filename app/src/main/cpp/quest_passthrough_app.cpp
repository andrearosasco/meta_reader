#include "quest_passthrough_app.hpp"
#include "quest_passthrough_common.hpp"
#include "hud_renderer.hpp"

#include <jni.h>

#include <array>
#include <chrono>
#include <thread>
#include <vector>

using namespace quest_passthrough;

namespace {

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
