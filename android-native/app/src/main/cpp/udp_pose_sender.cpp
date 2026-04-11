#include "udp_pose_sender.h"

#include <android/log.h>

#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

namespace vt::android {
namespace {

constexpr const char* kLogTag = "videotest-native";

} // namespace

UdpPoseSender::UdpPoseSender() = default;

UdpPoseSender::~UdpPoseSender() {
    Close();
}

bool UdpPoseSender::Open(const SenderConfig& config) {
    config_ = config;
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create UDP socket.");
        return false;
    }

    is_open_ = true;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "UDP sender opened for %s:%u",
                        config_.host.c_str(), static_cast<unsigned>(config_.port));
    return true;
}

void UdpPoseSender::Close() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    is_open_ = false;
}

bool UdpPoseSender::SendPose(const vt::proto::PacketHeader& header,
                             const vt::proto::PosePayload& payload) {
    if (!is_open_) {
        return false;
    }

    std::uint8_t packet[sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::PosePayload)]{};
    std::memcpy(packet, &header, sizeof(header));
    std::memcpy(packet + sizeof(header), &payload, sizeof(payload));

    return SendPacket(packet, sizeof(packet));
}

bool UdpPoseSender::SendControl(const vt::proto::PacketHeader& header,
                                const vt::proto::ControlPayload& payload) {
    if (!is_open_) {
        return false;
    }

    std::uint8_t packet[sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::ControlPayload)]{};
    std::memcpy(packet, &header, sizeof(header));
    std::memcpy(packet + sizeof(header), &payload, sizeof(payload));
    return SendPacket(packet, sizeof(packet));
}

bool UdpPoseSender::SendPacket(const std::uint8_t* packet, std::size_t packet_size) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.port);
    if (inet_pton(AF_INET, config_.host.c_str(), &addr.sin_addr) != 1) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Invalid target IP: %s", config_.host.c_str());
        return false;
    }

    const ssize_t sent = sendto(socket_fd_,
                                packet,
                                packet_size,
                                0,
                                reinterpret_cast<const sockaddr*>(&addr),
                                sizeof(addr));
    if (sent != static_cast<ssize_t>(packet_size)) {
        __android_log_print(ANDROID_LOG_WARN,
                            kLogTag,
                            "sendto failed for %s:%u errno=%d (%s)",
                            config_.host.c_str(),
                            static_cast<unsigned>(config_.port),
                            errno,
                            strerror(errno));
    }
    return sent == static_cast<ssize_t>(packet_size);
}

} // namespace vt::android
