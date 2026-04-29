#pragma once

#include <EGL/egl.h>
#include <jni.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct TelemetryState {
    std::string connectionState{"DISCOVERING"};
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

struct TrackedPoseState {
    bool tracked{false};
    XrPosef pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    float radius{0.0f};
};

struct HandTelemetryState {
    bool tracked{false};
    TrackedPoseState wrist;
    TrackedPoseState palm;
    std::array<TrackedPoseState, 5> fingertips{};
};

class TransportTelemetryBridge {
public:
    TransportTelemetryBridge();
    ~TransportTelemetryBridge();

    static void SetTransportModeSelection(bool wiredMode, uint16_t wiredPort);
    static void SetWirelessEndpointSelection(const std::string& host, uint16_t port);
    static void ClearWirelessEndpointSelection();

    TelemetryState SnapshotTelemetry() const;
    void PublishFrame(
        XrTime predictedDisplayTime,
        const HmdPoseState& headPose,
        const HandTelemetryState& leftHand,
        const HandTelemetryState& rightHand);

private:
    struct TelemetryFrameSample {
        uint64_t sequence{0};
        XrTime predictedDisplayTime{0};
        HmdPoseState headPose;
        HandTelemetryState leftHand;
        HandTelemetryState rightHand;
    };

    TransportSelection ReadSelection() const;
    void UpdateStatus(const TransportSelection& selection);
    void SyncConnection(const TransportSelection& selection);
    bool SendPacket(const std::vector<uint8_t>& packet);
    std::vector<uint8_t> SerializePacket(
        const TransportSelection& selection,
        const TelemetryFrameSample& frame,
        const TelemetryState& telemetry) const;
    void WorkerMain();
    bool OpenConnection(const TransportSelection& selection);
    void CloseConnection();
    static std::string DetectLocalIp();

    mutable std::mutex stateMutex_;
    std::condition_variable stateCondition_;
    TelemetryState telemetry_;
    std::optional<TelemetryFrameSample> pendingFrame_;
    uint64_t nextSequence_{0};
    bool stopRequested_{false};
    std::thread workerThread_;
    int transportSocket_{-1};
    bool transportSocketWiredMode_{false};
    std::string transportHost_;
    uint16_t transportPort_{0};
    std::chrono::steady_clock::time_point nextReconnectAttempt_{};
};
