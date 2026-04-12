#include "video_sender_core.h"

#include "control_protocol.h"
#include "packet_defs.h"
#include "video_protocol.h"

#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <thread>
#include <vector>

#include "NvEncoder/NvEncoderD3D11.h"

namespace {

constexpr std::size_t kChunkPayloadBytes = 1200;

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

void ConfigureNvencEncoder(const vt::windows::SenderRuntimeConfig& config,
                           NvEncoderD3D11* encoder,
                           NV_ENC_INITIALIZE_PARAMS* init_params,
                           NV_ENC_CONFIG* encode_config) {
    *init_params = {NV_ENC_INITIALIZE_PARAMS_VER};
    *encode_config = {NV_ENC_CONFIG_VER};
    init_params->encodeConfig = encode_config;
    encoder->CreateDefaultEncoderParams(init_params, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_LOW_LATENCY_HP_GUID);

    init_params->encodeWidth = config.width;
    init_params->encodeHeight = config.height;
    init_params->maxEncodeWidth = config.width;
    init_params->maxEncodeHeight = config.height;
    init_params->frameRateNum = std::max(config.fps, 1);
    init_params->frameRateDen = 1;
    init_params->enablePTD = 1;

    encode_config->gopLength = std::max(config.fps, 1);
    encode_config->frameIntervalP = 1;
    encode_config->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ;
    encode_config->rcParams.averageBitRate = config.bitrate;
    encode_config->rcParams.maxBitRate = config.bitrate;
    encode_config->rcParams.vbvBufferSize =
        std::max<std::uint32_t>(config.bitrate / static_cast<std::uint32_t>(std::max(config.fps, 1)), 1u);
    encode_config->rcParams.vbvInitialDelay = encode_config->rcParams.vbvBufferSize;
}

std::vector<std::uint8_t> FlattenAccessUnit(const std::vector<std::vector<std::uint8_t>>& packets) {
    std::vector<std::uint8_t> access_unit;
    for (const auto& packet : packets) {
        access_unit.insert(access_unit.end(), packet.begin(), packet.end());
    }
    return access_unit;
}

}  // namespace

namespace vt::windows {

bool OpenNonBlockingUdpSocket(std::uint16_t port, const char* label, SOCKET* out_socket) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << label << " socket() failed.\n";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << label << " bind() failed on port " << port << ".\n";
        closesocket(sock);
        return false;
    }

    u_long non_blocking = 1;
    if (ioctlsocket(sock, FIONBIO, &non_blocking) == SOCKET_ERROR) {
        std::cerr << label << " ioctlsocket(FIONBIO) failed.\n";
        closesocket(sock);
        return false;
    }

    *out_socket = sock;
    return true;
}

void HandleControlPayload(const vt::proto::ControlPayload& payload, ControlRequestState* state) {
    const auto message_type = static_cast<vt::proto::ControlMessageType>(payload.message_type);
    state->packets_received += 1;
    state->last_related_frame_id = payload.related_frame_id;

    if (message_type == vt::proto::ControlMessageType::RequestCodecConfig) {
        state->request_codec_config = true;
    } else if (message_type == vt::proto::ControlMessageType::RequestKeyframe) {
        state->request_keyframe = true;
    } else {
        return;
    }

    if (state->packets_received <= 5 || (state->packets_received % 60) == 0) {
        std::cout << "control request type=" << vt::proto::ControlMessageTypeName(message_type)
                  << " relatedFrame=" << payload.related_frame_id
                  << " requestId=" << payload.request_id
                  << " flags=0x" << std::hex << payload.flags << std::dec
                  << std::endl;
    }
}

void PollControlPackets(SOCKET sock, ControlRequestState* state) {
    while (true) {
        std::uint8_t buffer[1500]{};
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int bytes = recvfrom(sock,
                                   reinterpret_cast<char*>(buffer),
                                   static_cast<int>(sizeof(buffer)),
                                   0,
                                   reinterpret_cast<sockaddr*>(&from),
                                   &from_len);
        if (bytes == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                std::cerr << "recvfrom(control) failed: " << error << "\n";
            }
            break;
        }

        if (bytes < static_cast<int>(sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::ControlPayload))) {
            continue;
        }

        vt::proto::PacketHeader header{};
        std::memcpy(&header, buffer, sizeof(header));
        if (!vt::proto::IsValidHeader(header) ||
            header.type != static_cast<std::uint16_t>(vt::proto::PacketType::Control) ||
            header.payload_size != sizeof(vt::proto::ControlPayload)) {
            continue;
        }

        vt::proto::ControlPayload payload{};
        std::memcpy(&payload, buffer + sizeof(header), sizeof(payload));
        HandleControlPayload(payload, state);
    }
}

int RunNvencVideoSender(const SenderRuntimeConfig& requested_config,
                        IVideoContentSource* source,
                        const SenderRunOptions* options) {
    if (source == nullptr) {
        std::cerr << "RunNvencVideoSender requires a valid source.\n";
        return 1;
    }

    SenderRuntimeConfig config = requested_config;

    WSADATA wsa_data{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSAStartup failed.\n";
        return 1;
    }

    SOCKET video_socket = INVALID_SOCKET;
    int exit_code = 0;
    auto cleanup = [&]() {
        source->Shutdown();
        if (video_socket != INVALID_SOCKET) {
            closesocket(video_socket);
            video_socket = INVALID_SOCKET;
        }
        WSACleanup();
    };

    video_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (video_socket == INVALID_SOCKET) {
        std::cerr << "video socket() failed.\n";
        cleanup();
        return 1;
    }

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(config.video_port);
    if (inet_pton(AF_INET, config.host.c_str(), &target.sin_addr) != 1) {
        std::cerr << "Invalid target IP: " << config.host << "\n";
        cleanup();
        return 1;
    }

    if (!source->Initialize(&config.width, &config.height)) {
        cleanup();
        return 1;
    }

    try {
        NvEncoderD3D11 encoder(source->device(), config.width, config.height, NV_ENC_BUFFER_FORMAT_ARGB);

        NV_ENC_INITIALIZE_PARAMS init_params = {NV_ENC_INITIALIZE_PARAMS_VER};
        NV_ENC_CONFIG encode_config = {NV_ENC_CONFIG_VER};
        ConfigureNvencEncoder(config, &encoder, &init_params, &encode_config);
        encoder.CreateEncoder(&init_params);

        std::vector<std::uint8_t> sequence_params;
        std::uint32_t packet_frame_id = 0;
        const std::uint16_t source_frame_flags = source->EncodedFrameFlags();
        auto send_codec_config = [&](const char* reason) {
            sequence_params.clear();
            encoder.GetSequenceParams(sequence_params);
            if (sequence_params.empty()) {
                return;
            }

            ++packet_frame_id;
            SendEncodedFrame(video_socket,
                             target,
                             packet_frame_id,
                             config.width,
                             config.height,
                             static_cast<std::uint16_t>(
                                 vt::proto::VideoFrameFlagCodecConfig |
                                 vt::proto::VideoFrameFlagKeyframe |
                                 source_frame_flags),
                             sequence_params);
            std::cout << "sent codec config packet_id=" << packet_frame_id
                      << " bytes=" << sequence_params.size()
                      << " reason=" << reason
                      << std::endl;
        };

        send_codec_config("startup");

        const auto frame_interval = std::chrono::microseconds(1000000 / std::max(config.fps, 1));
        const auto start_time = std::chrono::steady_clock::now();
        const std::uint32_t keyframe_interval = static_cast<std::uint32_t>(std::max(config.fps, 1));
        std::uint32_t frame_index = 1;
        bool has_sent_video_access_unit = false;
        ControlRequestState control_state{};

        std::cout << source->SenderName() << " -> " << config.host << ":" << config.video_port
                  << " size=" << config.width << "x" << config.height
                  << " fps=" << config.fps
                  << " bitrate=" << config.bitrate;
        const std::string startup_details = source->StartupDetails();
        if (!startup_details.empty()) {
            std::cout << " " << startup_details;
        }
        std::cout << std::endl;

        while (true) {
            if (options != nullptr && options->stop_requested != nullptr && options->stop_requested->load()) {
                break;
            }

            const auto frame_start = std::chrono::steady_clock::now();
            FrameContext frame_context{};
            frame_context.frame_index = frame_index;
            frame_context.time_seconds = std::chrono::duration<double>(frame_start - start_time).count();

            ID3D11Texture2D* source_texture = nullptr;
            if (!source->UpdateAndRender(frame_context, &control_state, &source_texture) || source_texture == nullptr) {
                if (options != nullptr && options->stop_requested != nullptr && options->stop_requested->load()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }

            const auto* input_frame = encoder.GetNextInputFrame();
            auto* input_texture = reinterpret_cast<ID3D11Texture2D*>(const_cast<void*>(input_frame->inputPtr));
            source->BeforeEncodeCopy();
            source->context()->CopyResource(input_texture, source_texture);
            source->AfterEncodeCopy();

            if (control_state.request_codec_config) {
                send_codec_config("control_request");
            }
            if (control_state.request_keyframe || control_state.request_codec_config) {
                std::cout << "applying control requests keyframe="
                          << (control_state.request_keyframe ? "yes" : "no")
                          << " codec_config=" << (control_state.request_codec_config ? "yes" : "no")
                          << " relatedFrame=" << control_state.last_related_frame_id
                          << std::endl;
            }

            const bool force_idr =
                control_state.request_keyframe ||
                control_state.request_codec_config ||
                !has_sent_video_access_unit ||
                ((frame_index % keyframe_interval) == 0);

            std::vector<std::vector<std::uint8_t>> packets;
            NV_ENC_PIC_PARAMS pic_params = {NV_ENC_PIC_PARAMS_VER};
            if (force_idr) {
                pic_params.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
                encoder.EncodeFrame(packets, &pic_params);
            } else {
                encoder.EncodeFrame(packets, nullptr);
            }
            control_state.ClearPending();

            std::vector<std::uint8_t> access_unit = FlattenAccessUnit(packets);
            if (!access_unit.empty()) {
                const std::uint16_t flags = static_cast<std::uint16_t>(
                    (force_idr ? vt::proto::VideoFrameFlagKeyframe : vt::proto::VideoFrameFlagNone) |
                    source_frame_flags);
                ++packet_frame_id;
                SendEncodedFrame(
                    video_socket, target, packet_frame_id, config.width, config.height, flags, access_unit);
                has_sent_video_access_unit = true;
            }

            if (frame_index <= 5 || (frame_index % std::max(source->FrameLogInterval(), 1u)) == 0 || force_idr) {
                std::cout << "sent " << source->FrameLogLabel()
                          << " frame=" << frame_index
                          << " packet_id=" << packet_frame_id
                          << " bytes=" << access_unit.size()
                          << " keyframe=" << (force_idr ? "yes" : "no");
                const std::string suffix = source->FrameLogSuffix();
                if (!suffix.empty()) {
                    std::cout << " " << suffix;
                }
                std::cout << std::endl;
            }

            ++frame_index;
            const auto elapsed = std::chrono::steady_clock::now() - frame_start;
            if (elapsed < frame_interval) {
                std::this_thread::sleep_for(frame_interval - elapsed);
            }
        }
    } catch (const std::exception& ex) {
        std::cerr << source->SenderName() << " failed: " << ex.what() << std::endl;
        exit_code = 1;
    }

    cleanup();
    return exit_code;
}

}  // namespace vt::windows
