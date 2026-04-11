#include "udp_pose_receiver.h"

#include "packet_defs.h"
#include "pose_protocol.h"

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>

namespace vt::windows {
namespace {

constexpr int kMaxDatagramSize = 1500;
constexpr std::uint64_t kStatsReportIntervalMs = 1000;

struct ReceiverStats final {
    std::uint64_t packets_received = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t packets_since_last_report = 0;
    std::uint64_t bytes_since_last_report = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint32_t last_sequence = 0;
    bool has_last_sequence = false;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_report_at = started_at;
    std::string last_sender = "<none>";
};

void PrintPose(const vt::proto::PacketHeader& header, const vt::proto::PosePayload& pose) {
    std::cout << std::fixed << std::setprecision(4)
              << "[seq=" << header.sequence
              << " ts_us=" << header.timestamp_us
              << "] pos=(" << pose.position_m.x << ", " << pose.position_m.y << ", " << pose.position_m.z
              << ") quat=(" << pose.orientation.x << ", " << pose.orientation.y << ", "
              << pose.orientation.z << ", " << pose.orientation.w << ") flags=0x"
              << std::hex << pose.tracking_flags << std::dec << std::endl;
}

std::string FormatSender(const sockaddr_in& from) {
    char ip_buffer[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &from.sin_addr, ip_buffer, sizeof(ip_buffer));
    return std::string(ip_buffer) + ":" + std::to_string(ntohs(from.sin_port));
}

void MaybePrintStats(ReceiverStats& stats) {
    const auto now = std::chrono::steady_clock::now();
    const auto report_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - stats.last_report_at).count();
    if (report_elapsed < static_cast<long long>(kStatsReportIntervalMs)) {
        return;
    }

    const auto total_elapsed_sec =
        std::chrono::duration<double>(now - stats.started_at).count();
    const auto report_elapsed_sec =
        std::chrono::duration<double>(now - stats.last_report_at).count();
    const auto avg_pps =
        static_cast<double>(stats.packets_received) / (total_elapsed_sec > 0.0 ? total_elapsed_sec : 1.0);
    const auto avg_kbps = (static_cast<double>(stats.bytes_received) * 8.0 / 1000.0) /
                          (total_elapsed_sec > 0.0 ? total_elapsed_sec : 1.0);
    const auto window_pps =
        static_cast<double>(stats.packets_since_last_report) / (report_elapsed_sec > 0.0 ? report_elapsed_sec : 1.0);
    const auto window_kbps =
        (static_cast<double>(stats.bytes_since_last_report) * 8.0 / 1000.0) /
        (report_elapsed_sec > 0.0 ? report_elapsed_sec : 1.0);

    std::cout << "[stats] sender=" << stats.last_sender
              << " total_packets=" << stats.packets_received
              << " total_bytes=" << stats.bytes_received
              << " avg_pps=" << std::fixed << std::setprecision(1) << avg_pps
              << " avg_kbps=" << avg_kbps
              << " window_pps=" << window_pps
              << " window_kbps=" << window_kbps
              << " seq_gaps=" << stats.sequence_gaps
              << " report_window_s=" << report_elapsed_sec
              << std::endl;

    stats.last_report_at = now;
    stats.packets_since_last_report = 0;
    stats.bytes_since_last_report = 0;
}

} // namespace

int RunPoseReceiver(const ReceiverConfig& config) {
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

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "bind() failed.\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::array<std::uint8_t, kMaxDatagramSize> buffer{};
    ReceiverStats stats{};

    while (true) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int bytes = recvfrom(sock,
                                   reinterpret_cast<char*>(buffer.data()),
                                   static_cast<int>(buffer.size()),
                                   0,
                                   reinterpret_cast<sockaddr*>(&from),
                                   &from_len);
        if (bytes == SOCKET_ERROR) {
            std::cerr << "recvfrom() failed.\n";
            break;
        }

        stats.packets_received += 1;
        stats.bytes_received += static_cast<std::uint64_t>(bytes);
        stats.packets_since_last_report += 1;
        stats.bytes_since_last_report += static_cast<std::uint64_t>(bytes);
        stats.last_sender = FormatSender(from);

        if (bytes < static_cast<int>(sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::PosePayload))) {
            std::cerr << "Received packet too small: " << bytes << " bytes.\n";
            MaybePrintStats(stats);
            continue;
        }

        vt::proto::PacketHeader header{};
        std::memcpy(&header, buffer.data(), sizeof(header));

        if (!vt::proto::IsValidHeader(header)) {
            std::cerr << "Invalid packet header received.\n";
            continue;
        }

        if (header.type != static_cast<std::uint16_t>(vt::proto::PacketType::Pose)) {
            std::cerr << "Ignoring non-pose packet type: " << header.type << "\n";
            continue;
        }

        if (header.payload_size != sizeof(vt::proto::PosePayload)) {
            std::cerr << "Unexpected pose payload size: " << header.payload_size << "\n";
            continue;
        }

        vt::proto::PosePayload payload{};
        std::memcpy(&payload, buffer.data() + sizeof(header), sizeof(payload));

        if (stats.has_last_sequence && header.sequence > stats.last_sequence + 1) {
            stats.sequence_gaps += static_cast<std::uint64_t>(header.sequence - stats.last_sequence - 1);
        }
        stats.last_sequence = header.sequence;
        stats.has_last_sequence = true;

        if (stats.packets_received <= 5 || (stats.packets_received % 30) == 0) {
            std::cout << "[sender=" << stats.last_sender << "] ";
            PrintPose(header, payload);
        }

        MaybePrintStats(stats);
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}

} // namespace vt::windows
