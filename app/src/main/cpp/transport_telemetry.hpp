#pragma once

#include <EGL/egl.h>
#include <jni.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

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

    TelemetryState& telemetry() { return telemetry_; }
    const TelemetryState& telemetry() const { return telemetry_; }

    TransportSelection ReadSelection() const;
    void UpdateStatus(const TransportSelection& selection);
    void SyncConnection(const TransportSelection& selection);
    bool SendPacket(const std::vector<uint8_t>& packet);
    std::vector<uint8_t> SerializePacket(
        const TransportSelection& selection,
        XrTime predictedDisplayTime,
        const HmdPoseState& headPose,
        const HandTelemetryState& leftHand,
        const HandTelemetryState& rightHand) const;
    void NoteSuccessfulPacketSend();
    void ResetPacketRateIfStale();

private:
    bool OpenConnection(const TransportSelection& selection);
    void CloseConnection();
    static std::string DetectLocalIp();

    TelemetryState telemetry_;
    int transportSocket_{-1};
    bool transportSocketWiredMode_{false};
    std::string transportHost_;
    uint16_t transportPort_{0};
    mutable uint64_t telemetrySequence_{0};
    uint32_t packetWindowCount_{0};
    std::chrono::steady_clock::time_point packetWindowStart_{};
    std::chrono::steady_clock::time_point lastSuccessfulSendTime_{};
    std::chrono::steady_clock::time_point nextReconnectAttempt_{};
};