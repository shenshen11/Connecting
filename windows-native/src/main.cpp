#include "udp_pose_receiver.h"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    vt::windows::ReceiverConfig config{};

    if (argc >= 2) {
        try {
            config.port = static_cast<std::uint16_t>(std::stoul(argv[1]));
        } catch (const std::exception&) {
            std::cerr << "Invalid port argument. Using default " << config.port << ".\n";
        }
    }

    std::cout << "videotest pose_receiver starting on UDP port " << config.port << std::endl;
    std::cout << "Waiting for pose packets..." << std::endl;
    return vt::windows::RunPoseReceiver(config);
}
