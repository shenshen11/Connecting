#pragma once

#include "udp_encoded_video_receiver.h"

#include <android/native_window.h>

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <cstdint>
#include <vector>

namespace vt::android {

enum DecoderControlRequestFlags : std::uint32_t {
    DecoderControlRequestNone = 0,
    DecoderControlRequestKeyframe = 1 << 0,
    DecoderControlRequestCodecConfig = 1 << 1,
};

class AMediaH264Decoder final {
public:
    bool Initialize(ANativeWindow* output_window);
    void Shutdown();

    bool OnFrame(const EncodedVideoFrame& frame);
    void DrainOutput();

    bool IsConfigured() const noexcept { return configured_ && started_; }
    bool HasRenderedFrame() const noexcept { return has_rendered_frame_; }
    std::uint64_t QueuedFrameCount() const noexcept { return queued_input_frames_; }
    std::uint64_t RenderedFrameCount() const noexcept { return rendered_output_frames_; }
    std::uint16_t CurrentStreamFlags() const noexcept { return current_stream_flags_; }
    std::uint32_t ConsumePendingControlRequests(std::uint32_t* related_frame_id) noexcept;

private:
    bool ConfigureDecoder(std::uint16_t width, std::uint16_t height, const std::vector<std::uint8_t>& config_bytes);
    bool BootstrapDecoderFromFrame(const EncodedVideoFrame& frame);
    void ResetDecoder();
    void QueueControlRequests(std::uint32_t request_flags, std::uint32_t related_frame_id);
    void LogOutputFormat() const;
    static bool ExtractCsdFromAnnexB(const std::vector<std::uint8_t>& config_bytes,
                                     std::vector<std::uint8_t>* csd0,
                                     std::vector<std::uint8_t>* csd1);

    ANativeWindow* output_window_ = nullptr;
    AMediaCodec* codec_ = nullptr;
    bool configured_ = false;
    bool started_ = false;
    bool has_rendered_frame_ = false;
    std::uint16_t current_stream_flags_ = 0;
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    std::uint64_t received_frame_count_ = 0;
    std::uint64_t queued_input_frames_ = 0;
    std::uint64_t rendered_output_frames_ = 0;
    std::uint32_t last_queued_frame_id_ = 0;
    std::uint64_t dropped_before_config_count_ = 0;
    std::uint64_t dropped_no_input_buffer_count_ = 0;
    std::uint64_t dropped_input_buffer_too_small_count_ = 0;
    bool logged_waiting_for_first_output_ = false;
    std::uint32_t pending_control_requests_ = DecoderControlRequestNone;
    std::uint32_t pending_related_frame_id_ = 0;
    std::uint64_t last_keyframe_request_us_ = 0;
    std::uint64_t last_codec_config_request_us_ = 0;
};

}  // namespace vt::android
