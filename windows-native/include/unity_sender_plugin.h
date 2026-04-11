#pragma once

#include <cstdint>

#if defined(_WIN32)
#define VT_UNITY_SENDER_EXPORT extern "C" __declspec(dllexport)
#else
#define VT_UNITY_SENDER_EXPORT extern "C"
#endif

struct UnitySenderPose final {
    float position_m[3];
    float orientation_xyzw[4];
    std::uint32_t tracking_flags = 0;
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_us = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t sequence_gaps = 0;
    int has_pose = 0;
};

struct UnitySenderStats final {
    int unity_device_ready = 0;
    int source_texture_ready = 0;
    int copied_frame_ready = 0;
    int network_thread_running = 0;
    int sender_thread_running = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint64_t render_thread_copy_count = 0;
    std::uint64_t pose_packets_received = 0;
    std::uint64_t control_packets_received = 0;
    std::uint32_t last_pose_sequence = 0;
};

VT_UNITY_SENDER_EXPORT bool UnitySender_Configure(const char* target_host,
                                                  std::uint16_t video_port,
                                                  std::uint16_t pose_port,
                                                  int fps,
                                                  std::uint32_t bitrate,
                                                  std::uint16_t width,
                                                  std::uint16_t height);

VT_UNITY_SENDER_EXPORT void UnitySender_SetTexture(void* texture_handle);
VT_UNITY_SENDER_EXPORT bool UnitySender_Start();
VT_UNITY_SENDER_EXPORT void UnitySender_Stop();
VT_UNITY_SENDER_EXPORT bool UnitySender_IsRunning();

VT_UNITY_SENDER_EXPORT bool UnitySender_GetLatestPose(UnitySenderPose* out_pose);
VT_UNITY_SENDER_EXPORT bool UnitySender_GetStats(UnitySenderStats* out_stats);

VT_UNITY_SENDER_EXPORT int UnitySender_GetCopyTextureEventId();
VT_UNITY_SENDER_EXPORT void* UnitySender_GetRenderEventFunc();
