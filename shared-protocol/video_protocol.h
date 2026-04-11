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

enum VideoFrameFlags : std::uint16_t {
    VideoFrameFlagNone = 0,
    VideoFrameFlagCodecConfig = 1 << 0,
    VideoFrameFlagKeyframe = 1 << 1,
};

#pragma pack(push, 1)
struct VideoChunkHeader final {
    std::uint32_t frame_id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t pixel_format = static_cast<std::uint16_t>(VideoPixelFormat::Unknown);
    std::uint16_t flags = 0;
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
    std::uint32_t frame_size = 0;
    std::uint32_t chunk_offset = 0;
    std::uint16_t chunk_size = 0;
    std::uint16_t chunk_index = 0;
    std::uint16_t chunk_count = 0;
    std::uint16_t reserved = 0;
};
#pragma pack(pop)

}  // namespace vt::proto
