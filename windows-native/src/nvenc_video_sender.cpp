#include "packet_defs.h"
#include "video_protocol.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <d3d11.h>
#include <dxgi.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "NvEncoder/NvEncoderD3D11.h"

namespace {

constexpr std::size_t kChunkPayloadBytes = 1200;

struct SenderConfig final {
    std::string host = "127.0.0.1";
    std::uint16_t port = 25674;
    std::uint16_t width = 640;
    std::uint16_t height = 360;
    int fps = 30;
    std::uint32_t bitrate = 2'000'000;
};

void GenerateFrame(std::uint32_t frame_id,
                   std::uint16_t width,
                   std::uint16_t height,
                   std::vector<std::uint8_t>* bgra) {
    bgra->resize(static_cast<std::size_t>(width) * height * 4);
    const int box_size = 64;
    const int max_x = std::max<int>(0, static_cast<int>(width) - box_size);
    const int max_y = std::max<int>(0, static_cast<int>(height) - box_size);
    const int box_x = static_cast<int>((frame_id * 9) % std::max(1, max_x + 1));
    const int box_y = static_cast<int>((frame_id * 5) % std::max(1, max_y + 1));

    for (std::uint16_t y = 0; y < height; ++y) {
        for (std::uint16_t x = 0; x < width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
            std::uint8_t r = static_cast<std::uint8_t>((255 * x) / std::max<int>(1, width - 1));
            std::uint8_t g = static_cast<std::uint8_t>((255 * y) / std::max<int>(1, height - 1));
            std::uint8_t b = static_cast<std::uint8_t>((frame_id * 2) & 0xff);

            const bool inside_box =
                x >= box_x && x < (box_x + box_size) && y >= box_y && y < (box_y + box_size);
            if (inside_box) {
                r = 255;
                g = 255;
                b = 32;
            }

            (*bgra)[index + 0] = b;
            (*bgra)[index + 1] = g;
            (*bgra)[index + 2] = r;
            (*bgra)[index + 3] = 255;
        }
    }
}

void SendEncodedFrame(SOCKET sock,
                      const sockaddr_in& target,
                      std::uint32_t frame_id,
                      std::uint16_t width,
                      std::uint16_t height,
                      std::uint16_t flags,
                      const std::vector<std::uint8_t>& payload) {
    const std::uint32_t frame_size = static_cast<std::uint32_t>(payload.size());
    const std::uint16_t chunk_count =
        static_cast<std::uint16_t>((frame_size + kChunkPayloadBytes - 1) / kChunkPayloadBytes);

    for (std::uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const std::uint32_t chunk_offset = static_cast<std::uint32_t>(chunk_index * kChunkPayloadBytes);
        const std::uint16_t chunk_size = static_cast<std::uint16_t>(
            std::min<std::size_t>(kChunkPayloadBytes, payload.size() - chunk_offset));

        vt::proto::PacketHeader packet_header{};
        packet_header.type = static_cast<std::uint16_t>(vt::proto::PacketType::Video);
        packet_header.sequence = frame_id;
        packet_header.payload_size = sizeof(vt::proto::EncodedVideoChunkHeader) + chunk_size;
        packet_header.timestamp_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());

        vt::proto::EncodedVideoChunkHeader encoded_header{};
        encoded_header.frame_id = frame_id;
        encoded_header.width = width;
        encoded_header.height = height;
        encoded_header.codec = static_cast<std::uint16_t>(vt::proto::VideoCodec::H264AnnexB);
        encoded_header.flags = flags;
        encoded_header.frame_size = frame_size;
        encoded_header.chunk_offset = chunk_offset;
        encoded_header.chunk_size = chunk_size;
        encoded_header.chunk_index = chunk_index;
        encoded_header.chunk_count = chunk_count;

        std::vector<std::uint8_t> packet(sizeof(packet_header) + sizeof(encoded_header) + chunk_size);
        std::memcpy(packet.data(), &packet_header, sizeof(packet_header));
        std::memcpy(packet.data() + sizeof(packet_header), &encoded_header, sizeof(encoded_header));
        std::memcpy(packet.data() + sizeof(packet_header) + sizeof(encoded_header),
                    payload.data() + chunk_offset,
                    chunk_size);

        sendto(sock,
               reinterpret_cast<const char*>(packet.data()),
               static_cast<int>(packet.size()),
               0,
               reinterpret_cast<const sockaddr*>(&target),
               sizeof(target));
    }
}

}  // namespace

int main(int argc, char** argv) {
    SenderConfig config{};
    if (argc >= 2) config.host = argv[1];
    if (argc >= 3) {
        try { config.port = static_cast<std::uint16_t>(std::stoul(argv[2])); } catch (...) {}
    }
    if (argc >= 4) {
        try { config.width = static_cast<std::uint16_t>(std::stoul(argv[3])); } catch (...) {}
    }
    if (argc >= 5) {
        try { config.height = static_cast<std::uint16_t>(std::stoul(argv[4])); } catch (...) {}
    }
    if (argc >= 6) {
        try { config.fps = std::max(1, std::stoi(argv[5])); } catch (...) {}
    }
    if (argc >= 7) {
        try { config.bitrate = static_cast<std::uint32_t>(std::stoul(argv[6])); } catch (...) {}
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

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr,
                                   0,
                                   nullptr,
                                   0,
                                   D3D11_SDK_VERSION,
                                   &device,
                                   nullptr,
                                   &context);
    if (FAILED(hr) || device == nullptr || context == nullptr) {
        std::cerr << "D3D11CreateDevice failed.\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    try {
        NvEncoderD3D11 encoder(device, config.width, config.height, NV_ENC_BUFFER_FORMAT_ARGB);

        NV_ENC_INITIALIZE_PARAMS init_params = {NV_ENC_INITIALIZE_PARAMS_VER};
        NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
        init_params.encodeConfig = &encode_config;
        encoder.CreateDefaultEncoderParams(&init_params, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_LOW_LATENCY_HP_GUID);

        init_params.encodeWidth = config.width;
        init_params.encodeHeight = config.height;
        init_params.maxEncodeWidth = config.width;
        init_params.maxEncodeHeight = config.height;
        init_params.frameRateNum = config.fps;
        init_params.frameRateDen = 1;
        init_params.enablePTD = 1;

        encode_config.gopLength = config.fps;
        encode_config.frameIntervalP = 1;
        encode_config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ;
        encode_config.rcParams.averageBitRate = config.bitrate;
        encode_config.rcParams.maxBitRate = config.bitrate;
        encode_config.rcParams.vbvBufferSize = config.bitrate / config.fps;
        encode_config.rcParams.vbvInitialDelay = encode_config.rcParams.vbvBufferSize;

        encoder.CreateEncoder(&init_params);

        std::vector<std::uint8_t> sequence_params;
        encoder.GetSequenceParams(sequence_params);
        if (!sequence_params.empty()) {
            SendEncodedFrame(sock,
                             target,
                             1,
                             config.width,
                             config.height,
                             vt::proto::VideoFrameFlagCodecConfig | vt::proto::VideoFrameFlagKeyframe,
                             sequence_params);
            std::cout << "sent initial codec config packet_id=1 bytes=" << sequence_params.size() << std::endl;
        }

        std::vector<std::uint8_t> bgra;
        const auto frame_interval = std::chrono::microseconds(1000000 / config.fps);
        const std::uint32_t keyframe_interval = static_cast<std::uint32_t>(std::max(1, config.fps));
        std::uint32_t video_frame_id = 1;
        std::uint32_t packet_frame_id = sequence_params.empty() ? 0u : 1u;

        std::cout << "nvenc_video_sender -> " << config.host << ":" << config.port
                  << " size=" << config.width << "x" << config.height
                  << " fps=" << config.fps
                  << " bitrate=" << config.bitrate
                  << " keyframe_interval=" << keyframe_interval
                  << std::endl;

        while (true) {
            const auto frame_start = std::chrono::steady_clock::now();
            GenerateFrame(video_frame_id, config.width, config.height, &bgra);

            const auto* input_frame = encoder.GetNextInputFrame();
            auto* input_texture = reinterpret_cast<ID3D11Texture2D*>(const_cast<void*>(input_frame->inputPtr));
            context->UpdateSubresource(input_texture, 0, nullptr, bgra.data(), config.width * 4, 0);

            std::vector<std::vector<uint8_t>> packets;
            NV_ENC_PIC_PARAMS pic_params = {NV_ENC_PIC_PARAMS_VER};
            const bool force_idr = (video_frame_id == 1) || ((video_frame_id % keyframe_interval) == 0);
            if (force_idr) {
                pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
                encoder.EncodeFrame(packets, &pic_params);
            } else {
                encoder.EncodeFrame(packets, nullptr);
            }

            std::vector<std::uint8_t> access_unit;
            for (const auto& packet : packets) {
                access_unit.insert(access_unit.end(), packet.begin(), packet.end());
            }

            if (!access_unit.empty()) {
                const std::uint16_t flags = force_idr ? vt::proto::VideoFrameFlagKeyframe
                                                      : vt::proto::VideoFrameFlagNone;
                ++packet_frame_id;
                SendEncodedFrame(sock, target, packet_frame_id, config.width, config.height, flags, access_unit);
            }

            if (video_frame_id <= 5 || (video_frame_id % 30) == 0 || force_idr) {
                std::cout << "sent encoded video_frame=" << video_frame_id
                          << " packet_id=" << packet_frame_id
                          << " bytes=" << access_unit.size()
                          << " packets=" << packets.size()
                          << " keyframe=" << (force_idr ? "yes" : "no")
                          << std::endl;
            }

            ++video_frame_id;
            const auto elapsed = std::chrono::steady_clock::now() - frame_start;
            if (elapsed < frame_interval) {
                std::this_thread::sleep_for(frame_interval - elapsed);
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << "nvenc_video_sender failed: " << ex.what() << std::endl;
    }

    if (context != nullptr) context->Release();
    if (device != nullptr) device->Release();
    closesocket(sock);
    WSACleanup();
    return 0;
}
