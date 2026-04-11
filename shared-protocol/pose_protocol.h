#pragma once

#include <cstdint>

namespace vt::proto {

enum class TrackingFlags : std::uint32_t {
    None = 0,
    OrientationValid = 1 << 0,
    PositionValid = 1 << 1,
    OrientationTracked = 1 << 2,
    PositionTracked = 1 << 3,
};

inline constexpr TrackingFlags operator|(TrackingFlags lhs, TrackingFlags rhs) noexcept {
    return static_cast<TrackingFlags>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

inline constexpr bool HasFlag(TrackingFlags value, TrackingFlags flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

#pragma pack(push, 1)
struct Vec3f final {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quatf final {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct PosePayload final {
    Vec3f position_m;
    Quatf orientation;
    std::uint32_t tracking_flags = static_cast<std::uint32_t>(TrackingFlags::None);
    std::uint32_t reserved = 0;
};
#pragma pack(pop)

/*
Coordinate convention (phase 1 draft)
-------------------------------------
- Right-handed
- +Y up
- -Z forward
- Position in meters
- Quaternion order: x, y, z, w

This is a draft convention for phase 1. We will refine the exact mapping
when Android OpenXR sender is wired to the Windows receiver.
*/

} // namespace vt::proto
