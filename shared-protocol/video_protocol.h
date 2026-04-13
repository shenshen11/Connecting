#pragma once

#include <cstdint>

namespace vt::proto {

enum class VideoPixelFormat : std::uint16_t {
    Unknown = 0,
    Rgba8888 = 1,
};

enum class VideoCodec : std::uint16_t {
    Unknown = 0,
    RawRgba8888 = 1,
    H264AnnexB = 2,
};

enum class VideoStereoLayout : std::uint16_t {
    Mono = 0,
    ProjectionViews = 1,
    SideBySide = 2,
    TopBottom = 3,
};

inline constexpr const char* VideoStereoLayoutName(VideoStereoLayout layout) noexcept {
    switch (layout) {
        case VideoStereoLayout::ProjectionViews:
            return "projection_views";
        case VideoStereoLayout::SideBySide:
            return "side_by_side";
        case VideoStereoLayout::TopBottom:
            return "top_bottom";
        case VideoStereoLayout::Mono:
        default:
            return "mono";
    }
}

enum VideoFrameFlags : std::uint16_t {
    VideoFrameFlagNone = 0,
    VideoFrameFlagCodecConfig = 1 << 0,
    VideoFrameFlagKeyframe = 1 << 1,
    VideoFrameFlagVerticalFlip = 1 << 2,
};

#pragma pack(push, 1)
struct VideoStereoFrameMetadata final {
    std::uint8_t view_id = 0;
    std::uint8_t view_count = 1;
    std::uint16_t layout = static_cast<std::uint16_t>(VideoStereoLayout::Mono);
    std::uint32_t frame_pair_id = 0;
    std::int16_t reserved_fov_left_mdeg = 0;
    std::int16_t reserved_fov_right_mdeg = 0;
    std::int16_t reserved_fov_up_mdeg = 0;
    std::int16_t reserved_fov_down_mdeg = 0;
    std::int32_t reserved_projection_x = 0;
    std::int32_t reserved_projection_y = 0;
    std::int32_t reserved_projection_z = 0;
    std::int32_t reserved_projection_w = 0;
};

struct VideoChunkHeader final {
    std::uint32_t frame_id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t pixel_format = static_cast<std::uint16_t>(VideoPixelFormat::Unknown);
    std::uint16_t flags = 0;
    VideoStereoFrameMetadata stereo{};
    std::uint32_t frame_size = 0;
    std::uint32_t chunk_offset = 0;
    std::uint16_t chunk_size = 0;
    std::uint16_t chunk_index = 0;
    std::uint16_t chunk_count = 0;
    std::uint16_t reserved = 0;
};

struct EncodedVideoChunkHeader final {
    std::uint32_t frame_id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t codec = static_cast<std::uint16_t>(VideoCodec::Unknown);
    std::uint16_t flags = 0;
    VideoStereoFrameMetadata stereo{};
    std::uint32_t frame_size = 0;
    std::uint32_t chunk_offset = 0;
    std::uint16_t chunk_size = 0;
    std::uint16_t chunk_index = 0;
    std::uint16_t chunk_count = 0;
    std::uint16_t reserved = 0;
};
#pragma pack(pop)

inline constexpr VideoStereoFrameMetadata MakeMonoVideoStereoFrameMetadata(
    std::uint32_t frame_pair_id = 0) noexcept {
    VideoStereoFrameMetadata metadata{};
    metadata.view_id = 0;
    metadata.view_count = 1;
    metadata.layout = static_cast<std::uint16_t>(VideoStereoLayout::Mono);
    metadata.frame_pair_id = frame_pair_id;
    return metadata;
}

}  // namespace vt::proto
