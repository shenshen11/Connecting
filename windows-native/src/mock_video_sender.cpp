#include "packet_defs.h"
#include "video_protocol.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kChunkPayloadBytes = 1200;

struct SenderConfig final {
    std::string host = "127.0.0.1";
    std::uint16_t port = 25673;
    std::uint16_t width = 160;
    std::uint16_t height = 90;
    int fps = 10;
};

void GenerateFrame(std::uint32_t frame_id,
                   std::uint16_t width,
                   std::uint16_t height,
                   std::vector<std::uint8_t>* pixels) {
    pixels->resize(static_cast<std::size_t>(width) * height * 4);
    const int box_size = 36;
    const int max_x = std::max<int>(0, static_cast<int>(width) - box_size);
    const int max_y = std::max<int>(0, static_cast<int>(height) - box_size);
    const int box_x = static_cast<int>((frame_id * 5) % std::max(1, max_x + 1));
    const int box_y = static_cast<int>((frame_id * 3) % std::max(1, max_y + 1));

    for (std::uint16_t y = 0; y < height; ++y) {
        for (std::uint16_t x = 0; x < width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;

            std::uint8_t r = static_cast<std::uint8_t>((255 * x) / std::max<int>(1, width - 1));
            std::uint8_t g = static_cast<std::uint8_t>((255 * y) / std::max<int>(1, height - 1));
            std::uint8_t b = static_cast<std::uint8_t>((frame_id * 3) & 0xff);

            const bool inside_box = x >= box_x && x < (box_x + box_size) && y >= box_y && y < (box_y + box_size);
            if (inside_box) {
                r = 255;
                g = 255;
                b = 32;
            }

            (*pixels)[index + 0] = r;
            (*pixels)[index + 1] = g;
            (*pixels)[index + 2] = b;
            (*pixels)[index + 3] = 255;
        }
    }
}

}  // namespace

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
            config.width = static_cast<std::uint16_t>(std::stoul(argv[3]));
        } catch (...) {
        }
    }
    if (argc >= 5) {
        try {
            config.height = static_cast<std::uint16_t>(std::stoul(argv[4]));
        } catch (...) {
        }
    }
    if (argc >= 6) {
        try {
            config.fps = std::max(1, std::stoi(argv[5]));
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

    const auto frame_interval = std::chrono::microseconds(1000000 / config.fps);
    std::vector<std::uint8_t> frame_pixels;
    std::uint32_t frame_id = 0;

    std::cout << "mock_video_sender -> " << config.host << ":" << config.port
              << " size=" << config.width << "x" << config.height
              << " fps=" << config.fps << std::endl;

    while (true) {
        const auto frame_start = std::chrono::steady_clock::now();
        GenerateFrame(frame_id, config.width, config.height, &frame_pixels);

        const std::uint32_t frame_size = static_cast<std::uint32_t>(frame_pixels.size());
        const std::uint16_t chunk_count =
            static_cast<std::uint16_t>((frame_size + kChunkPayloadBytes - 1) / kChunkPayloadBytes);

        for (std::uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
            const std::uint32_t chunk_offset = static_cast<std::uint32_t>(chunk_index * kChunkPayloadBytes);
            const std::uint16_t chunk_size = static_cast<std::uint16_t>(
                std::min<std::size_t>(kChunkPayloadBytes, frame_pixels.size() - chunk_offset));

            vt::proto::PacketHeader packet_header{};
            packet_header.type = static_cast<std::uint16_t>(vt::proto::PacketType::Video);
            packet_header.sequence = frame_id;
            packet_header.payload_size = sizeof(vt::proto::VideoChunkHeader) + chunk_size;
            packet_header.timestamp_us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());

            vt::proto::VideoChunkHeader chunk_header{};
            chunk_header.frame_id = frame_id;
            chunk_header.width = config.width;
            chunk_header.height = config.height;
            chunk_header.pixel_format = static_cast<std::uint16_t>(vt::proto::VideoPixelFormat::Rgba8888);
            chunk_header.stereo = vt::proto::MakeMonoVideoStereoFrameMetadata(frame_id);
            chunk_header.frame_size = frame_size;
            chunk_header.chunk_offset = chunk_offset;
            chunk_header.chunk_size = chunk_size;
            chunk_header.chunk_index = chunk_index;
            chunk_header.chunk_count = chunk_count;

            std::vector<std::uint8_t> packet(sizeof(packet_header) + sizeof(chunk_header) + chunk_size);
            std::memcpy(packet.data(), &packet_header, sizeof(packet_header));
            std::memcpy(packet.data() + sizeof(packet_header), &chunk_header, sizeof(chunk_header));
            std::memcpy(packet.data() + sizeof(packet_header) + sizeof(chunk_header),
                        frame_pixels.data() + chunk_offset,
                        chunk_size);

            const int sent = sendto(sock,
                                    reinterpret_cast<const char*>(packet.data()),
                                    static_cast<int>(packet.size()),
                                    0,
                                    reinterpret_cast<const sockaddr*>(&target),
                                    sizeof(target));
            if (sent == SOCKET_ERROR) {
                std::cerr << "sendto() failed while sending frame " << frame_id
                          << " chunk " << chunk_index << "/" << chunk_count << "\n";
                closesocket(sock);
                WSACleanup();
                return 1;
            }
        }

        if (frame_id < 5 || (frame_id % 30) == 0) {
            std::cout << "sent video frame=" << frame_id
                      << " bytes=" << frame_size
                      << " chunks=" << chunk_count
                      << std::endl;
        }

        ++frame_id;
        const auto elapsed = std::chrono::steady_clock::now() - frame_start;
        if (elapsed < frame_interval) {
            std::this_thread::sleep_for(frame_interval - elapsed);
        }
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
