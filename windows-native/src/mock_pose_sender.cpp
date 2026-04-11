#include "packet_defs.h"
#include "pose_protocol.h"
#include "time_sync.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

struct SenderConfig final {
    std::string host = "127.0.0.1";
    std::uint16_t port = 25672;
    int hz = 75;
    std::uint32_t max_packets = 0;
};

vt::proto::PosePayload MakePose(std::uint32_t sequence) {
    using namespace std::chrono;
    const auto now = duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();

    vt::proto::PosePayload pose{};
    pose.position_m.x = static_cast<float>(std::sin(now * 1.0) * 0.05);
    pose.position_m.y = 1.60f + static_cast<float>(std::sin(now * 0.5) * 0.01);
    pose.position_m.z = static_cast<float>(std::cos(now * 1.0) * 0.05);
    pose.orientation.x = 0.0f;
    pose.orientation.y = static_cast<float>(std::sin(now * 0.5) * 0.2);
    pose.orientation.z = 0.0f;
    pose.orientation.w = 1.0f;
    pose.tracking_flags = static_cast<std::uint32_t>(
        vt::proto::TrackingFlags::OrientationValid |
        vt::proto::TrackingFlags::PositionValid |
        vt::proto::TrackingFlags::OrientationTracked |
        vt::proto::TrackingFlags::PositionTracked);
    pose.reserved = sequence;
    return pose;
}

} // namespace

int main(int argc, char** argv) {
    SenderConfig config{};
    if (argc >= 2) {
        config.host = argv[1];
    }
    if (argc >= 3) {
        try {
            config.port = static_cast<std::uint16_t>(std::stoul(argv[2]));
        } catch (...) {
        }
    }
    if (argc >= 4) {
        try {
            config.hz = std::max(1, std::stoi(argv[3]));
        } catch (...) {
        }
    }
    if (argc >= 5) {
        try {
            config.max_packets = static_cast<std::uint32_t>(std::stoul(argv[4]));
        } catch (...) {
        }
    }

    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup failed.\n";
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "socket() failed.\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(config.port);
    if (inet_pton(AF_INET, config.host.c_str(), &target.sin_addr) != 1) {
        std::cerr << "Invalid target IP: " << config.host << "\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    const auto frame_interval = std::chrono::microseconds(1000000 / config.hz);
    std::uint32_t sequence = 0;

    std::cout << "mock_pose_sender -> " << config.host << ":" << config.port
              << " @ " << config.hz << " Hz";
    if (config.max_packets > 0) {
        std::cout << " for " << config.max_packets << " packets";
    }
    std::cout << std::endl;

    while (true) {
        const auto frame_start = std::chrono::steady_clock::now();

        vt::proto::PacketHeader header{};
        header.type = static_cast<std::uint16_t>(vt::proto::PacketType::Pose);
        header.payload_size = sizeof(vt::proto::PosePayload);
        header.sequence = sequence++;
        header.timestamp_us = vt::proto::NowMicroseconds();

        const auto payload = MakePose(header.sequence);

        std::uint8_t packet[sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::PosePayload)]{};
        std::memcpy(packet, &header, sizeof(header));
        std::memcpy(packet + sizeof(header), &payload, sizeof(payload));

        const int sent = sendto(sock,
                                reinterpret_cast<const char*>(packet),
                                static_cast<int>(sizeof(packet)),
                                0,
                                reinterpret_cast<const sockaddr*>(&target),
                                sizeof(target));
        if (sent == SOCKET_ERROR) {
            std::cerr << "sendto() failed.\n";
            break;
        }

        if (header.sequence < 5 || (header.sequence % 60) == 0) {
            std::cout << "sent seq=" << header.sequence
                      << " ts_us=" << header.timestamp_us
                      << " pos=(" << payload.position_m.x << ", " << payload.position_m.y << ", " << payload.position_m.z
                      << ")" << std::endl;
        }

        if (config.max_packets > 0 && sequence >= config.max_packets) {
            std::cout << "mock_pose_sender finished after " << sequence << " packets." << std::endl;
            break;
        }

        const auto elapsed = std::chrono::steady_clock::now() - frame_start;
        if (elapsed < frame_interval) {
            std::this_thread::sleep_for(frame_interval - elapsed);
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
