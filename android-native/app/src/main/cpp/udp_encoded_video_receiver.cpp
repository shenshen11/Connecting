#include "udp_encoded_video_receiver.h"

#include <android/log.h>

#include <arpa/inet.h>
#include <algorithm>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace vt::android {
namespace {

constexpr const char* kLogTag = "videotest-native";
constexpr std::size_t kMaxPacketSize = 1500;
constexpr std::size_t kMaxQueuedFrames = 8;
constexpr std::size_t kMaxActiveFrameAssemblies = 32;
constexpr std::uint32_t kMaxFrameReorderWindow = 256;

bool IsNewerFrame(std::uint32_t incoming, std::uint32_t baseline) {
    return static_cast<std::int32_t>(incoming - baseline) > 0;
}

bool IsStreamRestartOrDiscontinuity(std::uint32_t incoming_frame_id,
                                    std::uint64_t incoming_timestamp_us,
                                    std::uint32_t baseline_frame_id,
                                    std::uint64_t baseline_timestamp_us) {
    if (baseline_timestamp_us == 0) {
        return false;
    }

    if (incoming_frame_id >= baseline_frame_id) {
        return false;
    }

    if (incoming_timestamp_us <= baseline_timestamp_us) {
        return false;
    }

    return true;
}

bool IsRecentOutOfOrderFrame(std::uint32_t incoming_frame_id, std::uint32_t baseline_frame_id) {
    if (incoming_frame_id >= baseline_frame_id) {
        return false;
    }

    return baseline_frame_id - incoming_frame_id <= kMaxFrameReorderWindow;
}

}  // namespace

UdpEncodedVideoReceiver::~UdpEncodedVideoReceiver() {
    Stop();
}

bool UdpEncodedVideoReceiver::Start(std::uint16_t port) {
    port_ = port;
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create UDP encoded-video socket.");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR,
                            kLogTag,
                            "Failed to bind UDP encoded-video port %u.",
                            static_cast<unsigned>(port_));
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    running_.store(true);
    thread_ = std::thread(&UdpEncodedVideoReceiver::ThreadMain, this);
    __android_log_print(
        ANDROID_LOG_INFO, kLogTag, "UDP encoded-video receiver listening on port %u", static_cast<unsigned>(port_));
    return true;
}

void UdpEncodedVideoReceiver::Stop() {
    running_.store(false);
    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
        close(socket_fd_);
        socket_fd_ = -1;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool UdpEncodedVideoReceiver::TryPopFrame(EncodedVideoFrame* out_frame) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (completed_frames_.empty()) {
        return false;
    }
    if (out_frame != nullptr) {
        *out_frame = std::move(completed_frames_.front());
    }
    completed_frames_.pop_front();
    return true;
}

void UdpEncodedVideoReceiver::ThreadMain() {
    std::vector<std::uint8_t> buffer(kMaxPacketSize);
    while (running_.load()) {
        const ssize_t bytes = recv(socket_fd_, buffer.data(), buffer.size(), 0);
        if (bytes <= 0) {
            continue;
        }
        HandlePacket(buffer.data(), static_cast<std::size_t>(bytes));
    }
}

void UdpEncodedVideoReceiver::HandlePacket(const std::uint8_t* data, std::size_t bytes) {
    const std::size_t header_bytes =
        sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::EncodedVideoChunkHeader);
    if (bytes < header_bytes) {
        return;
    }

    vt::proto::PacketHeader packet_header{};
    std::memcpy(&packet_header, data, sizeof(packet_header));
    if (!vt::proto::IsValidHeader(packet_header) ||
        packet_header.type != static_cast<std::uint16_t>(vt::proto::PacketType::Video)) {
        return;
    }

    vt::proto::EncodedVideoChunkHeader chunk_header{};
    std::memcpy(&chunk_header, data + sizeof(packet_header), sizeof(chunk_header));
    if (packet_header.payload_size != sizeof(vt::proto::EncodedVideoChunkHeader) + chunk_header.chunk_size) {
        return;
    }

    if (chunk_header.codec != static_cast<std::uint16_t>(vt::proto::VideoCodec::H264AnnexB)) {
        return;
    }

    if (chunk_header.chunk_count == 0 || chunk_header.frame_size == 0 ||
        chunk_header.chunk_offset + chunk_header.chunk_size > chunk_header.frame_size) {
        return;
    }

    auto assembly_iter = std::find_if(
        assemblies_.begin(),
        assemblies_.end(),
        [&](const FrameAssembly& assembly) {
            return assembly.active && assembly.frame_id == chunk_header.frame_id;
        });

    if (assembly_iter == assemblies_.end()) {
        if (has_completed_frame_ && !IsNewerFrame(chunk_header.frame_id, last_completed_frame_id_)) {
            if (chunk_header.frame_id == last_completed_frame_id_) {
                return;
            }

            const bool can_arrive_out_of_order =
                chunk_header.stereo.view_count > 1 &&
                chunk_header.stereo.layout ==
                    static_cast<std::uint16_t>(vt::proto::VideoStereoLayout::ProjectionViews);
            if (!can_arrive_out_of_order ||
                !IsRecentOutOfOrderFrame(chunk_header.frame_id, last_completed_frame_id_)) {
                if (!IsStreamRestartOrDiscontinuity(chunk_header.frame_id,
                                                    packet_header.timestamp_us,
                                                    last_completed_frame_id_,
                                                    last_completed_timestamp_us_)) {
                    return;
                }

                assemblies_.clear();
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    completed_frames_.clear();
                }
                ++stream_restart_count_;
                __android_log_print(
                    ANDROID_LOG_INFO,
                    kLogTag,
                    "Encoded video receiver detected sender restart/discontinuity #%u. lastFrame=%u lastTs=%llu incomingFrame=%u incomingTs=%llu. Resetting stale queued frames and accepting new stream.",
                    stream_restart_count_,
                    last_completed_frame_id_,
                    static_cast<unsigned long long>(last_completed_timestamp_us_),
                    chunk_header.frame_id,
                    static_cast<unsigned long long>(packet_header.timestamp_us));
                has_completed_frame_ = false;
                last_completed_frame_id_ = 0;
                last_completed_timestamp_us_ = 0;
            }
        }

        while (assemblies_.size() >= kMaxActiveFrameAssemblies) {
            assemblies_.pop_front();
        }
        assemblies_.push_back({});
        assembly_iter = assemblies_.end();
        --assembly_iter;

        assembly_iter->active = true;
        assembly_iter->frame_id = chunk_header.frame_id;
        assembly_iter->width = chunk_header.width;
        assembly_iter->height = chunk_header.height;
        assembly_iter->codec = vt::proto::VideoCodec::H264AnnexB;
        assembly_iter->flags = chunk_header.flags;
        assembly_iter->stereo = chunk_header.stereo;
        assembly_iter->timestamp_us = packet_header.timestamp_us;
        assembly_iter->latest_packet_timestamp_us = packet_header.timestamp_us;
        assembly_iter->frame_size = chunk_header.frame_size;
        assembly_iter->chunk_count = chunk_header.chunk_count;
        assembly_iter->received_chunks = 0;
        assembly_iter->bytes.assign(chunk_header.frame_size, 0);
        assembly_iter->chunk_received.assign(chunk_header.chunk_count, 0);
    } else if (packet_header.timestamp_us > assembly_iter->latest_packet_timestamp_us) {
        assembly_iter->latest_packet_timestamp_us = packet_header.timestamp_us;
    }

    if (chunk_header.chunk_index >= assembly_iter->chunk_received.size() ||
        assembly_iter->chunk_received[chunk_header.chunk_index] != 0) {
        return;
    }

    std::memcpy(assembly_iter->bytes.data() + chunk_header.chunk_offset,
                data + header_bytes,
                chunk_header.chunk_size);
    assembly_iter->chunk_received[chunk_header.chunk_index] = 1;
    assembly_iter->received_chunks += 1;

    if (assembly_iter->received_chunks == assembly_iter->chunk_count) {
        const std::uint32_t completed_frame_id = assembly_iter->frame_id;
        const std::uint64_t completed_timestamp_us = assembly_iter->latest_packet_timestamp_us;
        const vt::proto::VideoStereoFrameMetadata completed_stereo = assembly_iter->stereo;
        const std::uint32_t completed_frame_size = assembly_iter->frame_size;
        const std::uint16_t completed_flags = assembly_iter->flags;

        EncodedVideoFrame frame{};
        frame.frame_id = assembly_iter->frame_id;
        frame.width = assembly_iter->width;
        frame.height = assembly_iter->height;
        frame.codec = assembly_iter->codec;
        frame.flags = assembly_iter->flags;
        frame.stereo = assembly_iter->stereo;
        frame.timestamp_us = assembly_iter->timestamp_us;
        frame.bytes = std::move(assembly_iter->bytes);

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            completed_frames_.push_back(std::move(frame));
            while (completed_frames_.size() > kMaxQueuedFrames) {
                completed_frames_.pop_front();
            }
        }

        if (!has_completed_frame_ || IsNewerFrame(completed_frame_id, last_completed_frame_id_)) {
            last_completed_frame_id_ = completed_frame_id;
        }
        if (completed_timestamp_us > last_completed_timestamp_us_) {
            last_completed_timestamp_us_ = completed_timestamp_us;
        }
        has_completed_frame_ = true;
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "Encoded video frame complete id=%u pair=%u view=%u/%u layout=%s bytes=%u flags=0x%x",
                            completed_frame_id,
                            completed_stereo.frame_pair_id,
                            static_cast<unsigned>(completed_stereo.view_id),
                            static_cast<unsigned>(completed_stereo.view_count),
                            vt::proto::VideoStereoLayoutName(
                                static_cast<vt::proto::VideoStereoLayout>(completed_stereo.layout)),
                            completed_frame_size,
                            completed_flags);
        assemblies_.erase(assembly_iter);
    }
}

}  // namespace vt::android
