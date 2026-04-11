#pragma once

#include <chrono>
#include <cstdint>

namespace vt::proto {

inline std::uint64_t NowMicroseconds() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

} // namespace vt::proto
