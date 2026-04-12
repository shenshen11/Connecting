#include "amedia_h264_decoder.h"

#include "time_sync.h"

#include <android/log.h>

#include <algorithm>
#include <cstring>

namespace vt::android {
namespace {

constexpr const char* kLogTag = "videotest-native";
constexpr std::uint64_t kVerboseFrameCount = 5;
constexpr std::uint64_t kPeriodicFrameLogInterval = 30;
constexpr std::uint64_t kKeyframeRequestIntervalUs = 300000;
constexpr std::uint64_t kCodecConfigRequestIntervalUs = 500000;

bool FindStartCode(const std::vector<std::uint8_t>& bytes, std::size_t start, std::size_t* code_index, std::size_t* code_size) {
    for (std::size_t i = start; i + 3 < bytes.size(); ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0) {
            if (bytes[i + 2] == 1) {
                *code_index = i;
                *code_size = 3;
                return true;
            }
            if (i + 4 < bytes.size() && bytes[i + 2] == 0 && bytes[i + 3] == 1) {
                *code_index = i;
                *code_size = 4;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

bool AMediaH264Decoder::Initialize(ANativeWindow* output_window) {
    output_window_ = output_window;
    if (output_window_ != nullptr) {
        ANativeWindow_acquire(output_window_);
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "AMediaH264Decoder initialized with output window=%p", output_window_);
    }
    return output_window_ != nullptr;
}

void AMediaH264Decoder::Shutdown() {
    ResetDecoder();
    if (output_window_ != nullptr) {
        ANativeWindow_release(output_window_);
        output_window_ = nullptr;
    }
    has_rendered_frame_ = false;
}

bool AMediaH264Decoder::OnFrame(const EncodedVideoFrame& frame) {
    received_frame_count_ += 1;
    current_stream_flags_ = frame.flags;

    if (output_window_ == nullptr) {
        if (received_frame_count_ <= kVerboseFrameCount) {
            __android_log_print(ANDROID_LOG_WARN,
                                kLogTag,
                                "Dropping encoded frame id=%u because decoder output window is null.",
                                frame.frame_id);
        }
        return false;
    }

    if ((frame.flags & vt::proto::VideoFrameFlagCodecConfig) != 0) {
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "Received codec config frame id=%u bytes=%zu target=%ux%u flags=0x%x",
                            frame.frame_id,
                            frame.bytes.size(),
                            frame.width,
                            frame.height,
                            frame.flags);
        if (configured_ && codec_ != nullptr && frame.width == width_ && frame.height == height_) {
            __android_log_print(ANDROID_LOG_INFO,
                                kLogTag,
                                "Ignoring duplicate codec config for active decoder %ux%u.",
                                width_,
                                height_);
            return true;
        }
        return ConfigureDecoder(frame.width, frame.height, frame.bytes);
    }

    if (!configured_ || codec_ == nullptr) {
        const bool is_keyframe = (frame.flags & vt::proto::VideoFrameFlagKeyframe) != 0;
        if (is_keyframe && BootstrapDecoderFromFrame(frame)) {
            __android_log_print(ANDROID_LOG_INFO,
                                kLogTag,
                                "Bootstrapped decoder from keyframe id=%u; proceeding to queue same frame.",
                                frame.frame_id);
        } else {
            QueueControlRequests(DecoderControlRequestCodecConfig | DecoderControlRequestKeyframe, frame.frame_id);
            dropped_before_config_count_ += 1;
            if (dropped_before_config_count_ <= kVerboseFrameCount ||
                (dropped_before_config_count_ % kPeriodicFrameLogInterval) == 0) {
                __android_log_print(
                    ANDROID_LOG_WARN,
                    kLogTag,
                    "Dropping encoded frame id=%u bytes=%zu because decoder is not configured yet. keyframe=%s dropCount=%llu",
                    frame.frame_id,
                    frame.bytes.size(),
                    is_keyframe ? "yes" : "no",
                    static_cast<unsigned long long>(dropped_before_config_count_));
            }
            return false;
        }
    }

    const ssize_t input_index = AMediaCodec_dequeueInputBuffer(codec_, 0);
    if (input_index < 0) {
        dropped_no_input_buffer_count_ += 1;
        if (dropped_no_input_buffer_count_ <= kVerboseFrameCount ||
            (dropped_no_input_buffer_count_ % kPeriodicFrameLogInterval) == 0) {
            __android_log_print(ANDROID_LOG_WARN,
                                kLogTag,
                                "No MediaCodec input buffer available for frame id=%u. dropCount=%llu",
                                frame.frame_id,
                                static_cast<unsigned long long>(dropped_no_input_buffer_count_));
        }
        DrainOutput();
        return false;
    }

    size_t input_size = 0;
    std::uint8_t* input_buffer = AMediaCodec_getInputBuffer(codec_, input_index, &input_size);
    if (input_buffer == nullptr || input_size < frame.bytes.size()) {
        dropped_input_buffer_too_small_count_ += 1;
        __android_log_print(ANDROID_LOG_WARN,
                            kLogTag,
                            "MediaCodec input buffer invalid for frame id=%u. inputIndex=%zd inputSize=%zu frameBytes=%zu dropCount=%llu",
                            frame.frame_id,
                            input_index,
                            input_size,
                            frame.bytes.size(),
                            static_cast<unsigned long long>(dropped_input_buffer_too_small_count_));
        AMediaCodec_queueInputBuffer(codec_, input_index, 0, 0, 0, 0);
        return false;
    }

    std::memcpy(input_buffer, frame.bytes.data(), frame.bytes.size());
    uint32_t flags = 0;
    if ((frame.flags & vt::proto::VideoFrameFlagKeyframe) != 0) {
        flags |= AMEDIACODEC_BUFFER_FLAG_KEY_FRAME;
    }

    media_status_t status = AMediaCodec_queueInputBuffer(codec_,
                                                         input_index,
                                                         0,
                                                         frame.bytes.size(),
                                                         static_cast<int64_t>(frame.timestamp_us),
                                                         flags);
    if (status != AMEDIA_OK) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "AMediaCodec_queueInputBuffer failed: %d", status);
        return false;
    }

    queued_input_frames_ += 1;
    last_queued_frame_id_ = frame.frame_id;
    logged_waiting_for_first_output_ = false;
    if (queued_input_frames_ <= kVerboseFrameCount || (queued_input_frames_ % kPeriodicFrameLogInterval) == 0) {
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "Queued encoded frame id=%u bytes=%zu ptsUs=%llu flags=0x%x inputIndex=%zd queued=%llu",
                            frame.frame_id,
                            frame.bytes.size(),
                            static_cast<unsigned long long>(frame.timestamp_us),
                            flags,
                            input_index,
                            static_cast<unsigned long long>(queued_input_frames_));
    }

    DrainOutput();
    return true;
}

void AMediaH264Decoder::DrainOutput() {
    if (codec_ == nullptr || !started_) {
        return;
    }

    AMediaCodecBufferInfo info{};
    while (true) {
        const ssize_t output_index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 0);
        if (output_index >= 0) {
            const bool render = info.size > 0;
            const media_status_t release_status = AMediaCodec_releaseOutputBuffer(codec_, output_index, render);
            if (release_status != AMEDIA_OK) {
                __android_log_print(ANDROID_LOG_WARN,
                                    kLogTag,
                                    "AMediaCodec_releaseOutputBuffer failed: %d index=%zd",
                                    release_status,
                                    output_index);
            }
            if (render) {
                has_rendered_frame_ = true;
                rendered_output_frames_ += 1;
                if (rendered_output_frames_ == 1) {
                    __android_log_print(ANDROID_LOG_INFO,
                                        kLogTag,
                                        "MediaCodec rendered first output frame. ptsUs=%lld size=%d flags=0x%x",
                                        static_cast<long long>(info.presentationTimeUs),
                                        static_cast<int>(info.size),
                                        info.flags);
                } else if (rendered_output_frames_ <= kVerboseFrameCount ||
                           (rendered_output_frames_ % kPeriodicFrameLogInterval) == 0) {
                    __android_log_print(ANDROID_LOG_INFO,
                                        kLogTag,
                                        "Rendered output frame #%llu ptsUs=%lld size=%d flags=0x%x",
                                        static_cast<unsigned long long>(rendered_output_frames_),
                                        static_cast<long long>(info.presentationTimeUs),
                                        static_cast<int>(info.size),
                                        info.flags);
                }
            }
            continue;
        }
        if (output_index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            LogOutputFormat();
            continue;
        }
        if (output_index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            if (!has_rendered_frame_ && queued_input_frames_ > 0 && !logged_waiting_for_first_output_) {
                __android_log_print(ANDROID_LOG_INFO,
                                    kLogTag,
                                    "MediaCodec is waiting for first output. queuedInputs=%llu renderedOutputs=%llu",
                                    static_cast<unsigned long long>(queued_input_frames_),
                                    static_cast<unsigned long long>(rendered_output_frames_));
                logged_waiting_for_first_output_ = true;
            }
            if (!has_rendered_frame_ && queued_input_frames_ >= 3) {
                QueueControlRequests(DecoderControlRequestKeyframe, last_queued_frame_id_);
            }
            return;
        }
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "Unexpected AMediaCodec_dequeueOutputBuffer result=%zd", output_index);
        return;
    }
}

bool AMediaH264Decoder::ConfigureDecoder(std::uint16_t width,
                                         std::uint16_t height,
                                         const std::vector<std::uint8_t>& config_bytes) {
    std::vector<std::uint8_t> csd0;
    std::vector<std::uint8_t> csd1;
    if (!ExtractCsdFromAnnexB(config_bytes, &csd0, &csd1)) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "Failed to extract SPS/PPS from codec config.");
        return false;
    }

    ResetDecoder();

    codec_ = AMediaCodec_createDecoderByType("video/avc");
    if (codec_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "AMediaCodec_createDecoderByType failed.");
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "Configuring MediaCodec for %ux%u. csd-0=%zu bytes csd-1=%zu bytes",
                        width,
                        height,
                        csd0.size(),
                        csd1.size());

    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
    AMediaFormat_setBuffer(format, "csd-0", csd0.data(), csd0.size());
    AMediaFormat_setBuffer(format, "csd-1", csd1.data(), csd1.size());

    const media_status_t configure_status = AMediaCodec_configure(codec_, format, output_window_, nullptr, 0);
    AMediaFormat_delete(format);
    if (configure_status != AMEDIA_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "AMediaCodec_configure failed: %d", configure_status);
        ResetDecoder();
        return false;
    }

    const media_status_t start_status = AMediaCodec_start(codec_);
    if (start_status != AMEDIA_OK) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "AMediaCodec_start failed: %d", start_status);
        ResetDecoder();
        return false;
    }

    configured_ = true;
    started_ = true;
    width_ = width;
    height_ = height;
    has_rendered_frame_ = false;
    logged_waiting_for_first_output_ = false;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "MediaCodec configured for %ux%u H.264", width_, height_);
    LogOutputFormat();
    return true;
}

bool AMediaH264Decoder::BootstrapDecoderFromFrame(const EncodedVideoFrame& frame) {
    if ((frame.flags & vt::proto::VideoFrameFlagKeyframe) == 0) {
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "Attempting decoder bootstrap from keyframe id=%u bytes=%zu",
                        frame.frame_id,
                        frame.bytes.size());
    if (!ConfigureDecoder(frame.width, frame.height, frame.bytes)) {
        __android_log_print(ANDROID_LOG_WARN,
                            kLogTag,
                            "Failed to bootstrap decoder from keyframe id=%u.",
                            frame.frame_id);
        return false;
    }
    return true;
}

void AMediaH264Decoder::ResetDecoder() {
    configured_ = false;
    started_ = false;
    current_stream_flags_ = 0;
    width_ = 0;
    height_ = 0;
    queued_input_frames_ = 0;
    rendered_output_frames_ = 0;
    last_queued_frame_id_ = 0;
    dropped_before_config_count_ = 0;
    dropped_no_input_buffer_count_ = 0;
    dropped_input_buffer_too_small_count_ = 0;
    logged_waiting_for_first_output_ = false;
    pending_control_requests_ = DecoderControlRequestNone;
    pending_related_frame_id_ = 0;
    last_keyframe_request_us_ = 0;
    last_codec_config_request_us_ = 0;

    if (codec_ != nullptr) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
    }
}

void AMediaH264Decoder::QueueControlRequests(std::uint32_t request_flags, std::uint32_t related_frame_id) {
    const std::uint64_t now_us = vt::proto::NowMicroseconds();
    std::uint32_t accepted_flags = DecoderControlRequestNone;

    if ((request_flags & DecoderControlRequestKeyframe) != 0 &&
        (last_keyframe_request_us_ == 0 || (now_us - last_keyframe_request_us_) >= kKeyframeRequestIntervalUs)) {
        accepted_flags |= DecoderControlRequestKeyframe;
        last_keyframe_request_us_ = now_us;
    }

    if ((request_flags & DecoderControlRequestCodecConfig) != 0 &&
        (last_codec_config_request_us_ == 0 ||
         (now_us - last_codec_config_request_us_) >= kCodecConfigRequestIntervalUs)) {
        accepted_flags |= DecoderControlRequestCodecConfig;
        last_codec_config_request_us_ = now_us;
    }

    if (accepted_flags == DecoderControlRequestNone) {
        return;
    }

    pending_control_requests_ |= accepted_flags;
    pending_related_frame_id_ = related_frame_id;

    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "Queued decoder control request flags=0x%x relatedFrame=%u",
                        accepted_flags,
                        related_frame_id);
}

std::uint32_t AMediaH264Decoder::ConsumePendingControlRequests(std::uint32_t* related_frame_id) noexcept {
    if (related_frame_id != nullptr) {
        *related_frame_id = pending_related_frame_id_;
    }
    const std::uint32_t flags = pending_control_requests_;
    pending_control_requests_ = DecoderControlRequestNone;
    pending_related_frame_id_ = 0;
    return flags;
}

void AMediaH264Decoder::LogOutputFormat() const {
    if (codec_ == nullptr) {
        return;
    }

    AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
    if (format == nullptr) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "AMediaCodec output format is null.");
        return;
    }

    const char* format_text = AMediaFormat_toString(format);
    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "AMediaCodec output format: %s",
                        (format_text != nullptr) ? format_text : "(null)");
    AMediaFormat_delete(format);
}

bool AMediaH264Decoder::ExtractCsdFromAnnexB(const std::vector<std::uint8_t>& config_bytes,
                                             std::vector<std::uint8_t>* csd0,
                                             std::vector<std::uint8_t>* csd1) {
    std::size_t search_from = 0;
    while (search_from < config_bytes.size()) {
        std::size_t start_index = 0;
        std::size_t start_code_size = 0;
        if (!FindStartCode(config_bytes, search_from, &start_index, &start_code_size)) {
            break;
        }

        std::size_t next_index = config_bytes.size();
        std::size_t next_code_size = 0;
        FindStartCode(config_bytes, start_index + start_code_size, &next_index, &next_code_size);

        if (start_index + start_code_size < config_bytes.size()) {
            const std::uint8_t nal_type = config_bytes[start_index + start_code_size] & 0x1f;
            const auto nal_begin = config_bytes.begin() + static_cast<std::ptrdiff_t>(start_index);
            const auto nal_end = config_bytes.begin() + static_cast<std::ptrdiff_t>(next_index);
            if (nal_type == 7) {
                csd0->assign(nal_begin, nal_end);
            } else if (nal_type == 8) {
                csd1->assign(nal_begin, nal_end);
            }
        }

        search_from = next_index;
        if (search_from >= config_bytes.size()) {
            break;
        }
    }

    return !csd0->empty() && !csd1->empty();
}

}  // namespace vt::android
