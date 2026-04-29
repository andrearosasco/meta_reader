#include "transport_telemetry.hpp"

#include <android/log.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {

constexpr const char* kLogTag = "MetaReaderXR";
constexpr const char* kWiredLoopbackHost = "127.0.0.1";
constexpr uint16_t kDefaultWiredLoopbackPort = 5005;
constexpr auto kReconnectDelay = std::chrono::milliseconds(1000);
constexpr int kConnectTimeoutMs = 150;
constexpr std::array<const char*, 5> kFingertipNames{{
    "thumb_tip",
    "index_tip",
    "middle_tip",
    "ring_tip",
    "little_tip",
}};

void LogInfo(const std::string& message) {
    __android_log_write(ANDROID_LOG_INFO, kLogTag, message.c_str());
}

void LogError(const std::string& message) {
    __android_log_write(ANDROID_LOG_ERROR, kLogTag, message.c_str());
}

struct TransportSelectionState {
    bool wiredMode{false};
    uint16_t wiredPort{kDefaultWiredLoopbackPort};
    bool wirelessEndpointAvailable{false};
    std::string wirelessHost;
    uint16_t wirelessPort{0};
};

std::mutex gTransportSelectionMutex;
TransportSelectionState gTransportSelectionState;

std::string EndpointToString(const std::string& host, uint16_t port) {
    if (host.empty()) {
        return "SEARCHING";
    }

    std::ostringstream stream;
    stream << host;
    if (port > 0) {
        stream << ':' << port;
    }
    return stream.str();
}

bool WriteAllToSocket(int socketFd, const std::vector<uint8_t>& packet) {
    size_t offset = 0;
    while (offset < packet.size()) {
        const ssize_t bytesSent = send(
            socketFd,
            packet.data() + offset,
            packet.size() - offset,
            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (bytesSent < 0 && errno == EINTR) {
            continue;
        }
        if (bytesSent <= 0) {
            return false;
        }
        offset += static_cast<size_t>(bytesSent);
    }
    return true;
}

bool ConfigureSocketNonBlocking(int socketFd) {
    const int flags = fcntl(socketFd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(socketFd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void AppendVector3Json(std::ostringstream& stream, const XrVector3f& value) {
    stream << '[' << value.x << ',' << value.y << ',' << value.z << ']';
}

void AppendQuaternionJson(std::ostringstream& stream, const XrQuaternionf& value) {
    stream << '[' << value.x << ',' << value.y << ',' << value.z << ',' << value.w << ']';
}

void AppendPoseJson(std::ostringstream& stream, const XrPosef& pose) {
    stream << '{' << "\"position\":";
    AppendVector3Json(stream, pose.position);
    stream << ",\"orientation\":";
    AppendQuaternionJson(stream, pose.orientation);
    stream << '}';
}

void AppendTrackedPoseJson(std::ostringstream& stream, const TrackedPoseState& pose) {
    stream << "{\"tracked\":" << (pose.tracked ? "true" : "false");
    if (pose.tracked) {
        stream << ",\"pose\":";
        AppendPoseJson(stream, pose.pose);
        if (pose.radius > 0.0f) {
            stream << ",\"radius\":" << pose.radius;
        }
    }
    stream << '}';
}

std::vector<uint8_t> MakeLengthPrefixedPacket(const std::string& payload) {
    const uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    std::vector<uint8_t> packet(4 + payloadSize);
    packet[0] = static_cast<uint8_t>((payloadSize >> 24) & 0xff);
    packet[1] = static_cast<uint8_t>((payloadSize >> 16) & 0xff);
    packet[2] = static_cast<uint8_t>((payloadSize >> 8) & 0xff);
    packet[3] = static_cast<uint8_t>(payloadSize & 0xff);
    std::memcpy(packet.data() + 4, payload.data(), payloadSize);
    return packet;
}

}  // namespace

TransportTelemetryBridge::TransportTelemetryBridge() {
    telemetry_.localIp = DetectLocalIp();
    workerThread_ = std::thread(&TransportTelemetryBridge::WorkerMain, this);
}

TransportTelemetryBridge::~TransportTelemetryBridge() {
    {
        std::scoped_lock lock(stateMutex_);
        stopRequested_ = true;
    }
    stateCondition_.notify_all();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    CloseConnection();
}

TelemetryState TransportTelemetryBridge::SnapshotTelemetry() const {
    std::scoped_lock lock(stateMutex_);
    return telemetry_;
}

void TransportTelemetryBridge::PublishFrame(
    XrTime predictedDisplayTime,
    const HmdPoseState& headPose,
    const HandTelemetryState& leftHand,
    const HandTelemetryState& rightHand) {
    {
        std::scoped_lock lock(stateMutex_);
        pendingFrame_ = TelemetryFrameSample{
            .sequence = nextSequence_++,
            .predictedDisplayTime = predictedDisplayTime,
            .headPose = headPose,
            .leftHand = leftHand,
            .rightHand = rightHand,
        };
        telemetry_.trackingValid = headPose.valid;
    }
    stateCondition_.notify_one();
}

void TransportTelemetryBridge::SetTransportModeSelection(bool wiredMode, uint16_t wiredPort) {
    std::scoped_lock lock(gTransportSelectionMutex);
    gTransportSelectionState.wiredMode = wiredMode;
    gTransportSelectionState.wiredPort = wiredPort > 0 ? wiredPort : kDefaultWiredLoopbackPort;
    if (wiredMode) {
        gTransportSelectionState.wirelessEndpointAvailable = false;
        gTransportSelectionState.wirelessHost.clear();
        gTransportSelectionState.wirelessPort = 0;
    }
}

void TransportTelemetryBridge::SetWirelessEndpointSelection(const std::string& host, uint16_t port) {
    std::scoped_lock lock(gTransportSelectionMutex);
    gTransportSelectionState.wirelessEndpointAvailable = !host.empty() && port > 0;
    gTransportSelectionState.wirelessHost = host;
    gTransportSelectionState.wirelessPort = port;
}

void TransportTelemetryBridge::ClearWirelessEndpointSelection() {
    std::scoped_lock lock(gTransportSelectionMutex);
    gTransportSelectionState.wirelessEndpointAvailable = false;
    gTransportSelectionState.wirelessHost.clear();
    gTransportSelectionState.wirelessPort = 0;
}

TransportSelection TransportTelemetryBridge::ReadSelection() const {
    std::scoped_lock lock(gTransportSelectionMutex);

    TransportSelection selection;
    selection.wiredMode = gTransportSelectionState.wiredMode;
    if (selection.wiredMode) {
        selection.hasEndpoint = true;
        selection.host = kWiredLoopbackHost;
        selection.port = gTransportSelectionState.wiredPort;
        return selection;
    }

    selection.hasEndpoint = gTransportSelectionState.wirelessEndpointAvailable;
    selection.host = gTransportSelectionState.wirelessHost;
    selection.port = gTransportSelectionState.wirelessPort;
    return selection;
}

void TransportTelemetryBridge::UpdateStatus(const TransportSelection& selection) {
    std::scoped_lock lock(stateMutex_);
    telemetry_.targetHost = selection.hasEndpoint ? EndpointToString(selection.host, selection.port) : "SEARCHING";
    if (selection.wiredMode) {
        telemetry_.connectionState = transportSocket_ >= 0 ? "WIRED CONNECTED" : "WIRED WAITING";
    } else if (!selection.hasEndpoint) {
        telemetry_.connectionState = "WIRELESS SEARCH";
    } else {
        telemetry_.connectionState = transportSocket_ >= 0 ? "WIRELESS CONNECTED" : "WIRELESS WAIT";
    }
}

void TransportTelemetryBridge::SyncConnection(const TransportSelection& selection) {
    const bool selectionChanged =
        transportSocket_ < 0 ||
        transportSocketWiredMode_ != selection.wiredMode ||
        transportHost_ != selection.host ||
        transportPort_ != selection.port;

    if (!selection.hasEndpoint) {
        CloseConnection();
        return;
    }
    if (!selectionChanged) {
        return;
    }

    CloseConnection();

    const auto now = std::chrono::steady_clock::now();
    if (now < nextReconnectAttempt_) {
        return;
    }
    if (OpenConnection(selection)) {
        nextReconnectAttempt_ = std::chrono::steady_clock::time_point{};
    } else {
        nextReconnectAttempt_ = now + kReconnectDelay;
    }
}

bool TransportTelemetryBridge::SendPacket(const std::vector<uint8_t>& packet) {
    if (transportSocket_ < 0) {
        return false;
    }

    const bool success = transportSocketWiredMode_
        ? WriteAllToSocket(transportSocket_, packet)
        : send(transportSocket_, packet.data(), packet.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(packet.size());
    if (success) {
        return true;
    }

    std::ostringstream stream;
    stream << (transportSocketWiredMode_ ? "Wired TCP send failed for " : "Wireless UDP send failed for ")
           << EndpointToString(transportHost_, transportPort_)
           << " (errno=" << errno << ')';
    LogError(stream.str());
    CloseConnection();
    nextReconnectAttempt_ = std::chrono::steady_clock::now() + kReconnectDelay;
    return false;
}

std::vector<uint8_t> TransportTelemetryBridge::SerializePacket(
    const TransportSelection& selection,
    const TelemetryFrameSample& frame,
    const TelemetryState& telemetry) const {
    std::ostringstream payloadStream;
    payloadStream << std::fixed << std::setprecision(6);

    const auto appendHandJson = [&](const HandTelemetryState& hand) {
        payloadStream << '{'
                      << "\"tracked\":" << (hand.tracked ? "true" : "false")
                      << ",\"wrist\":";
        AppendTrackedPoseJson(payloadStream, hand.wrist);
        payloadStream << ",\"palm\":";
        AppendTrackedPoseJson(payloadStream, hand.palm);
        payloadStream << ",\"fingertips\":{";
        for (size_t index = 0; index < hand.fingertips.size(); ++index) {
            if (index > 0) {
                payloadStream << ',';
            }
            payloadStream << '"' << kFingertipNames[index] << "\":";
            AppendTrackedPoseJson(payloadStream, hand.fingertips[index]);
        }
        payloadStream << "}}";
    };

    payloadStream << '{'
                  << "\"type\":\"quest_telemetry\"," 
                  << "\"version\":1,"
                  << "\"sequence\":" << frame.sequence << ','
                  << "\"transport\":\"" << (selection.wiredMode ? "wired" : "wireless") << "\"," 
                  << "\"display_time_ns\":" << static_cast<long long>(frame.predictedDisplayTime) << ','
                  << "\"tracking_valid\":" << (frame.headPose.valid ? "true" : "false") << ','
                  << "\"head_pose\":{"
                  << "\"valid\":" << (frame.headPose.valid ? "true" : "false") << ','
                  << "\"position\":";
    AppendVector3Json(payloadStream, frame.headPose.position);
    payloadStream << ",\"orientation\":";
    AppendQuaternionJson(payloadStream, frame.headPose.orientation);
    payloadStream << "},\"hands\":{";
    payloadStream << "\"left\":";
    appendHandJson(frame.leftHand);
    payloadStream << ",\"right\":";
    appendHandJson(frame.rightHand);
    payloadStream << "},\"status\":{"
                  << "\"connection_state\":\"" << telemetry.connectionState << "\"," 
                  << "\"target_host\":\"" << telemetry.targetHost << "\"}}";
    return MakeLengthPrefixedPacket(payloadStream.str());
}

void TransportTelemetryBridge::WorkerMain() {
    while (true) {
        std::optional<TelemetryFrameSample> frame;
        {
            std::unique_lock lock(stateMutex_);
            stateCondition_.wait(lock, [&] {
                return stopRequested_ || pendingFrame_.has_value();
            });
            if (stopRequested_) {
                break;
            }
            frame = std::move(pendingFrame_);
            pendingFrame_.reset();
        }

        if (!frame.has_value()) {
            continue;
        }

        const TransportSelection selection = ReadSelection();
        UpdateStatus(selection);
        if (!selection.hasEndpoint) {
            CloseConnection();
            UpdateStatus(selection);
            continue;
        }

        SyncConnection(selection);
        UpdateStatus(selection);
        if (transportSocket_ < 0) {
            continue;
        }

        const TelemetryState telemetry = SnapshotTelemetry();
        const std::vector<uint8_t> packet = SerializePacket(selection, *frame, telemetry);
        SendPacket(packet);
        UpdateStatus(selection);
    }
}

bool TransportTelemetryBridge::OpenConnection(const TransportSelection& selection) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(selection.port);
    if (inet_pton(AF_INET, selection.host.c_str(), &address.sin_addr) != 1) {
        LogError("Transport connection failed: invalid address " + selection.host);
        return false;
    }

    const int socketFd = socket(AF_INET, selection.wiredMode ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (socketFd < 0) {
        LogError("Transport connection failed: unable to create socket.");
        return false;
    }

    const auto fail = [&](const std::string& message) {
        close(socketFd);
        LogError(message);
        return false;
    };

    if (!ConfigureSocketNonBlocking(socketFd)) {
        return fail("Transport connection failed: unable to configure non-blocking transport socket.");
    }

    if (selection.wiredMode) {
        const int connectResult = connect(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        if (connectResult != 0 && errno != EINPROGRESS) {
            return fail("Wired TCP connection failed to " + EndpointToString(selection.host, selection.port) + " (errno=" + std::to_string(errno) + ")");
        }

        if (connectResult != 0) {
            pollfd pollDescriptor{};
            pollDescriptor.fd = socketFd;
            pollDescriptor.events = POLLOUT;
            if (poll(&pollDescriptor, 1, kConnectTimeoutMs) <= 0) {
                return fail("Wired TCP connection timed out for " + EndpointToString(selection.host, selection.port));
            }

            int socketError = 0;
            socklen_t socketErrorSize = sizeof(socketError);
            if (getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &socketError, &socketErrorSize) != 0 || socketError != 0) {
                return fail("Wired TCP connection failed for " + EndpointToString(selection.host, selection.port) + " (errno=" + std::to_string(socketError) + ")");
            }
        }

        const int disableNagle = 1;
        if (setsockopt(socketFd, IPPROTO_TCP, TCP_NODELAY, &disableNagle, sizeof(disableNagle)) != 0) {
            LogError("Unable to disable TCP_NODELAY for wired transport.");
        }
    } else if (connect(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return fail("Wireless UDP connection setup failed for " + EndpointToString(selection.host, selection.port) + " (errno=" + std::to_string(errno) + ")");
    }

    transportSocket_ = socketFd;
    transportSocketWiredMode_ = selection.wiredMode;
    transportHost_ = selection.host;
    transportPort_ = selection.port;
    LogInfo(std::string(selection.wiredMode ? "Using wired ADB reverse TCP path: " : "Using wireless Avahi UDP path: ") + EndpointToString(selection.host, selection.port));
    return true;
}

void TransportTelemetryBridge::CloseConnection() {
    if (transportSocket_ >= 0) {
        close(transportSocket_);
        transportSocket_ = -1;
    }
    transportSocketWiredMode_ = false;
    transportHost_.clear();
    transportPort_ = 0;
}

std::string TransportTelemetryBridge::DetectLocalIp() {
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
