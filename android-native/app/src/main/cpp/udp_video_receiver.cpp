#include "udp_video_receiver.h"

#include <android/log.h>

#include <arpa/inet.h>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace vt::android {
namespace {

constexpr const char* kLogTag = "videotest-native";
constexpr std::size_t kMaxPacketSize = 1500;

bool IsNewerFrame(std::uint32_t incoming, std::uint32_t baseline) {
    return static_cast<std::int32_t>(incoming - baseline) > 0;
}

}  // namespace

UdpVideoReceiver::~UdpVideoReceiver() {
    Stop();
}

bool UdpVideoReceiver::Start(std::uint16_t port) {
    port_ = port;
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to create UDP video socket.");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Failed to bind UDP video port %u.", static_cast<unsigned>(port_));
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    running_.store(true);
    thread_ = std::thread(&UdpVideoReceiver::ThreadMain, this);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "UDP video receiver listening on port %u", static_cast<unsigned>(port_));
    return true;
}

void UdpVideoReceiver::Stop() {
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

bool UdpVideoReceiver::TryConsumeLatestFrame(VideoFrame* out_frame) {
    std::lock_guard<std::mutex> lock(latest_mutex_);
    if (!has_latest_frame_) {
        return false;
    }
    if (out_frame != nullptr) {
        *out_frame = latest_frame_;
    }
    has_latest_frame_ = false;
    return true;
}

void UdpVideoReceiver::ThreadMain() {
    std::vector<std::uint8_t> buffer(kMaxPacketSize);
    while (running_.load()) {
        const ssize_t bytes = recv(socket_fd_, buffer.data(), buffer.size(), 0);
        if (bytes <= 0) {
            continue;
        }
        HandlePacket(buffer.data(), static_cast<std::size_t>(bytes));
    }
}

void UdpVideoReceiver::HandlePacket(const std::uint8_t* data, std::size_t bytes) {
    const std::size_t header_bytes = sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::VideoChunkHeader);
    if (bytes < header_bytes) {
        return;
    }

    vt::proto::PacketHeader packet_header{};
    std::memcpy(&packet_header, data, sizeof(packet_header));
    if (!vt::proto::IsValidHeader(packet_header) ||
        packet_header.type != static_cast<std::uint16_t>(vt::proto::PacketType::Video)) {
        return;
    }

    vt::proto::VideoChunkHeader chunk_header{};
    std::memcpy(&chunk_header, data + sizeof(packet_header), sizeof(chunk_header));
    if (packet_header.payload_size != sizeof(vt::proto::VideoChunkHeader) + chunk_header.chunk_size) {
        return;
    }

    if (chunk_header.pixel_format != static_cast<std::uint16_t>(vt::proto::VideoPixelFormat::Rgba8888)) {
        return;
    }
    if (chunk_header.chunk_count == 0 || chunk_header.frame_size == 0) {
        return;
    }
    if (chunk_header.chunk_offset + chunk_header.chunk_size > chunk_header.frame_size) {
        return;
    }

    if (!assembly_.active || assembly_.frame_id != chunk_header.frame_id) {
        if (has_completed_frame_ && !IsNewerFrame(chunk_header.frame_id, last_completed_frame_id_)) {
            return;
        }

        assembly_.active = true;
        assembly_.frame_id = chunk_header.frame_id;
        assembly_.width = chunk_header.width;
        assembly_.height = chunk_header.height;
        assembly_.pixel_format = vt::proto::VideoPixelFormat::Rgba8888;
        assembly_.frame_size = chunk_header.frame_size;
        assembly_.chunk_count = chunk_header.chunk_count;
        assembly_.received_chunks = 0;
        assembly_.pixels.assign(chunk_header.frame_size, 0);
        assembly_.chunk_received.assign(chunk_header.chunk_count, 0);
    }

    if (chunk_header.chunk_index >= assembly_.chunk_received.size()) {
        return;
    }
    if (assembly_.chunk_received[chunk_header.chunk_index] != 0) {
        return;
    }

    std::memcpy(assembly_.pixels.data() + chunk_header.chunk_offset,
                data + header_bytes,
                chunk_header.chunk_size);
    assembly_.chunk_received[chunk_header.chunk_index] = 1;
    assembly_.received_chunks += 1;

    if (assembly_.received_chunks == assembly_.chunk_count) {
        VideoFrame completed{};
        completed.frame_id = assembly_.frame_id;
        completed.width = assembly_.width;
        completed.height = assembly_.height;
        completed.pixel_format = assembly_.pixel_format;
        completed.pixels = std::move(assembly_.pixels);

        {
            std::lock_guard<std::mutex> lock(latest_mutex_);
            latest_frame_ = std::move(completed);
            has_latest_frame_ = true;
        }

        last_completed_frame_id_ = assembly_.frame_id;
        has_completed_frame_ = true;
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "Video frame complete id=%u size=%ux%u bytes=%u",
                            last_completed_frame_id_,
                            assembly_.width,
                            assembly_.height,
                            assembly_.frame_size);
        assembly_ = {};
    }
}

}  // namespace vt::android
