#pragma once

#include "control_protocol.h"
#include "packet_defs.h"
#include "pose_protocol.h"

#include <cstdint>
#include <string>

namespace vt::android {

struct SenderConfig final {
    std::string host = "192.168.1.100";
    std::uint16_t port = 25672;
};

class UdpPoseSender final {
public:
    UdpPoseSender();
    ~UdpPoseSender();

    bool Open(const SenderConfig& config);
    void Close();
    bool SendPose(const vt::proto::PacketHeader& header, const vt::proto::PosePayload& payload);
    bool SendControl(const vt::proto::PacketHeader& header, const vt::proto::ControlPayload& payload);

private:
    bool SendPacket(const std::uint8_t* packet, std::size_t packet_size);

    int socket_fd_ = -1;
    bool is_open_ = false;
    SenderConfig config_{};
};

} // namespace vt::android
