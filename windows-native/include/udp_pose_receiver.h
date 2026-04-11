#pragma once

#include <cstdint>
#include <string>

namespace vt::windows {

struct ReceiverConfig final {
    std::uint16_t port = 25672;
    std::string bind_address = "0.0.0.0";
};

int RunPoseReceiver(const ReceiverConfig& config);

} // namespace vt::windows
