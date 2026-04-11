#pragma once

#include <cstdint>

namespace vt::proto {

enum class ControlMessageType : std::uint16_t {
    Unknown = 0,
    RequestKeyframe = 1,
    RequestCodecConfig = 2,
    Heartbeat = 3,
};

enum ControlMessageFlags : std::uint16_t {
    ControlMessageFlagNone = 0,
    ControlMessageFlagUrgent = 1 << 0,
};

#pragma pack(push, 1)
struct ControlPayload final {
    std::uint16_t message_type = static_cast<std::uint16_t>(ControlMessageType::Unknown);
    std::uint16_t flags = ControlMessageFlagNone;
    std::uint32_t request_id = 0;
    std::uint32_t related_frame_id = 0;
    std::uint32_t value0 = 0;
    std::uint32_t value1 = 0;
};
#pragma pack(pop)

inline constexpr const char* ControlMessageTypeName(ControlMessageType type) noexcept {
    switch (type) {
        case ControlMessageType::RequestKeyframe:
            return "RequestKeyframe";
        case ControlMessageType::RequestCodecConfig:
            return "RequestCodecConfig";
        case ControlMessageType::Heartbeat:
            return "Heartbeat";
        default:
            return "Unknown";
    }
}

}  // namespace vt::proto
