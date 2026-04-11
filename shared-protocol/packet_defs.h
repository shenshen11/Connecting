#pragma once

#include <cstdint>

namespace vt::proto {

constexpr std::uint32_t kMagic = 0x56525431; // "VRT1"
constexpr std::uint16_t kProtocolVersion = 1;

enum class PacketType : std::uint16_t {
    Unknown = 0,
    Pose = 1,
    Control = 2,
    Video = 3,
};

#pragma pack(push, 1)
struct PacketHeader final {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kProtocolVersion;
    std::uint16_t type = static_cast<std::uint16_t>(PacketType::Unknown);
    std::uint32_t payload_size = 0;
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_us = 0;
};
#pragma pack(pop)

inline constexpr bool IsValidHeader(const PacketHeader& header) noexcept {
    return header.magic == kMagic && header.version == kProtocolVersion;
}

} // namespace vt::proto
