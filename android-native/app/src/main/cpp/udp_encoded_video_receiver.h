#pragma once

#include "packet_defs.h"
#include "video_protocol.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace vt::android {

struct EncodedVideoFrame final {
    std::uint32_t frame_id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    vt::proto::VideoCodec codec = vt::proto::VideoCodec::Unknown;
    std::uint16_t flags = 0;
    vt::proto::VideoStereoFrameMetadata stereo{};
    std::uint64_t timestamp_us = 0;
    std::vector<std::uint8_t> bytes;
};

class UdpEncodedVideoReceiver final {
public:
    UdpEncodedVideoReceiver() = default;
    ~UdpEncodedVideoReceiver();

    bool Start(std::uint16_t port);
    void Stop();

    bool TryPopFrame(EncodedVideoFrame* out_frame);

private:
    struct FrameAssembly final {
        bool active = false;
        std::uint32_t frame_id = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
        vt::proto::VideoCodec codec = vt::proto::VideoCodec::Unknown;
        std::uint16_t flags = 0;
        vt::proto::VideoStereoFrameMetadata stereo{};
        std::uint64_t timestamp_us = 0;
        std::uint64_t latest_packet_timestamp_us = 0;
        std::uint32_t frame_size = 0;
        std::uint16_t chunk_count = 0;
        std::uint16_t received_chunks = 0;
        std::vector<std::uint8_t> bytes;
        std::vector<std::uint8_t> chunk_received;
    };

    void ThreadMain();
    void HandlePacket(const std::uint8_t* data, std::size_t bytes);

    int socket_fd_ = -1;
    std::uint16_t port_ = 25674;
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex queue_mutex_;
    std::deque<EncodedVideoFrame> completed_frames_;

    std::deque<FrameAssembly> assemblies_;
    std::uint32_t last_completed_frame_id_ = 0;
    std::uint64_t last_completed_timestamp_us_ = 0;
    bool has_completed_frame_ = false;
    std::uint32_t stream_restart_count_ = 0;
};

}  // namespace vt::android
