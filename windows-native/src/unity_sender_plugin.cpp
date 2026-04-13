#include "unity_sender_plugin.h"

#include "control_protocol.h"
#include "packet_defs.h"
#include "pose_protocol.h"
#include "video_protocol.h"
#include "video_sender_core.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <array>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D11.h"
#include "IUnityInterface.h"

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kRenderEventCopyTexture = 0;
constexpr std::size_t kMaxStereoViews = 2;

const char* DxgiFormatName(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM:
            return "DXGI_FORMAT_B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return "DXGI_FORMAT_B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return "DXGI_FORMAT_R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
        default:
            return "DXGI_FORMAT_OTHER";
    }
}

bool IsNvencArgbCopyCompatibleFormat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

class ImmediateContextGuard final {
public:
    explicit ImmediateContextGuard(ID3D11Multithread* multithread) : multithread_(multithread) {
        if (multithread_ != nullptr) {
            multithread_->Enter();
        }
    }

    ~ImmediateContextGuard() {
        if (multithread_ != nullptr) {
            multithread_->Leave();
        }
    }

    ImmediateContextGuard(const ImmediateContextGuard&) = delete;
    ImmediateContextGuard& operator=(const ImmediateContextGuard&) = delete;

private:
    ID3D11Multithread* multithread_ = nullptr;
};

void NormalizeQuaternion(vt::proto::Quatf* q) {
    const float len_sq = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    if (len_sq <= 1e-8f) {
        q->x = 0.0f;
        q->y = 0.0f;
        q->z = 0.0f;
        q->w = 1.0f;
        return;
    }

    const float inv_len = 1.0f / std::sqrt(len_sq);
    q->x *= inv_len;
    q->y *= inv_len;
    q->z *= inv_len;
    q->w *= inv_len;
}

struct LatestPoseState final {
    vt::proto::PosePayload pose{};
    std::uint32_t sequence = 0;
    std::uint64_t timestamp_us = 0;
    std::uint64_t packets_received = 0;
    std::uint64_t sequence_gaps = 0;
    bool has_pose = false;
};

struct ViewTextureState final {
    void* pending_texture_handle = nullptr;
    void* current_texture_handle = nullptr;
    ComPtr<ID3D11Texture2D> source_texture;
    ComPtr<ID3D11Texture2D> copied_texture;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    DXGI_FORMAT source_format = DXGI_FORMAT_UNKNOWN;
    bool source_format_logged = false;
    bool source_format_error_logged = false;
};

bool IsValidViewId(int view_id) {
    return view_id >= 0 && view_id < static_cast<int>(kMaxStereoViews);
}

class UnitySenderPluginState;
void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType event_type);
void UNITY_INTERFACE_API OnRenderEvent(int event_id);

class UnityTextureContentSource final : public vt::windows::IVideoContentSource {
public:
    UnityTextureContentSource(UnitySenderPluginState* owner, std::uint8_t view_id);

    bool Initialize(std::uint16_t* inout_width, std::uint16_t* inout_height) override;
    bool UpdateAndRender(const vt::windows::FrameContext& frame_context,
                         vt::windows::ControlRequestState* control_state,
                         ID3D11Texture2D** out_texture) override;

    ID3D11Device* device() const noexcept override;
    ID3D11DeviceContext* context() const noexcept override;
    const char* SenderName() const noexcept override { return "unity_sender_plugin"; }
    const char* FrameLogLabel() const noexcept override { return frame_log_label_.c_str(); }
    std::uint32_t FrameLogInterval() const noexcept override { return 60; }
    std::string StartupDetails() const override;
    std::string FrameLogSuffix() const override;
    std::uint16_t EncodedFrameFlags() const noexcept override {
        return vt::proto::VideoFrameFlagVerticalFlip;
    }
    vt::proto::VideoStereoFrameMetadata EncodedCodecConfigStereoMetadata() const noexcept override;
    vt::proto::VideoStereoFrameMetadata EncodedFrameStereoMetadata(
        const vt::windows::FrameContext& frame_context) const noexcept override;
    void BeforeEncodeCopy() override;
    void AfterEncodeCopy() override;
    void Shutdown() noexcept override;

private:
    UnitySenderPluginState* owner_ = nullptr;
    std::uint8_t view_id_ = 0;
    std::string frame_log_label_;
    std::uint32_t last_acquired_pair_id_ = 0;
    std::uint64_t last_control_generation_ = 0;
};

class UnitySenderPluginState final {
public:
    UnitySenderPluginState() {
        runtime_config_.width = 0;
        runtime_config_.height = 0;
    }

    void SetInterfaces(IUnityInterfaces* interfaces) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        unity_interfaces_ = interfaces;
        unity_graphics_ = interfaces != nullptr ? interfaces->Get<IUnityGraphics>() : nullptr;
        unity_graphics_d3d11_ = interfaces != nullptr ? interfaces->Get<IUnityGraphicsD3D11>() : nullptr;
        if (unity_graphics_ != nullptr) {
            reserved_event_base_ = unity_graphics_->ReserveEventIDRange(1);
        }
    }

    void OnGraphicsDeviceEvent(UnityGfxDeviceEventType event_type) {
        if (event_type == kUnityGfxDeviceEventInitialize) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            renderer_ = unity_graphics_ != nullptr ? unity_graphics_->GetRenderer() : kUnityGfxRendererNull;
            if (renderer_ != kUnityGfxRendererD3D11 || unity_graphics_d3d11_ == nullptr) {
                std::cerr << "unity_sender_plugin requires Unity D3D11 renderer.\n";
                unity_device_.Reset();
                unity_context_.Reset();
                return;
            }

            ID3D11Device* device = unity_graphics_d3d11_->GetDevice();
            if (device == nullptr) {
                std::cerr << "Unity D3D11 device is null during initialization.\n";
                unity_device_.Reset();
                unity_context_.Reset();
                return;
            }

            unity_device_ = device;
            ID3D11DeviceContext* immediate_context = nullptr;
            unity_device_->GetImmediateContext(&immediate_context);
            unity_context_.Attach(immediate_context);

            ComPtr<ID3D11Multithread> multithread;
            if (SUCCEEDED(unity_context_.As(&multithread)) && multithread != nullptr) {
                const BOOL was_protected = multithread->SetMultithreadProtected(TRUE);
                unity_multithread_ = multithread;
                std::cout << "unity_sender_plugin enabled D3D11 multithread protection"
                          << " previous=" << (was_protected ? "on" : "off") << "\n";
            } else {
                std::cerr << "unity_sender_plugin could not query ID3D11Multithread from Unity immediate context.\n";
                unity_multithread_.Reset();
            }

            std::cout << "unity_sender_plugin initialized with Unity D3D11 device.\n";
            return;
        }

        if (event_type == kUnityGfxDeviceEventShutdown) {
            Stop();

            std::lock_guard<std::mutex> state_lock(state_mutex_);
            renderer_ = kUnityGfxRendererNull;
            unity_context_.Reset();
            unity_multithread_.Reset();
            unity_device_.Reset();
            configured_view_count_ = 0;

            std::lock_guard<std::mutex> texture_lock(texture_mutex_);
            for (std::size_t view_index = 0; view_index < kMaxStereoViews; ++view_index) {
                ClearViewTextureStateLocked(view_index);
            }
            active_encode_width_ = 0;
            active_encode_height_ = 0;
            render_thread_copy_count_ = 0;
            return;
        }
    }

    void UnregisterDeviceCallback() {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (unity_graphics_ != nullptr) {
            unity_graphics_->UnregisterDeviceEventCallback(::OnGraphicsDeviceEvent);
        }
    }

    bool Configure(const char* target_host,
                   std::uint16_t video_port,
                   std::uint16_t pose_port,
                   int fps,
                   std::uint32_t bitrate,
                   std::uint16_t width,
                   std::uint16_t height) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (sender_thread_running_.load() || network_thread_running_.load()) {
            std::cerr << "UnitySender_Configure requires the plugin to be stopped.\n";
            return false;
        }

        if (target_host != nullptr && target_host[0] != '\0') {
            runtime_config_.host = target_host;
        }
        if (video_port != 0) {
            runtime_config_.video_port = video_port;
        }
        if (pose_port != 0) {
            pose_port_ = pose_port;
        }
        runtime_config_.fps = std::max(fps, 1);
        runtime_config_.bitrate = bitrate != 0 ? bitrate : runtime_config_.bitrate;
        runtime_config_.width = width;
        runtime_config_.height = height;
        return true;
    }

    void SetTextureHandle(void* texture_handle) {
        SetTextureHandleForView(0, texture_handle);
    }

    void SetTextureHandleForView(int view_id, void* texture_handle) {
        if (!IsValidViewId(view_id)) {
            std::cerr << "UnitySender_SetTextureForView received invalid view id " << view_id << ".\n";
            return;
        }

        std::lock_guard<std::mutex> lock(texture_mutex_);
        if (texture_handle == nullptr) {
            ClearViewTextureStateLocked(static_cast<std::size_t>(view_id));
            return;
        }

        view_textures_[view_id].pending_texture_handle = texture_handle;
    }

    bool Start() {
        bool should_auto_learn_target = false;
        std::uint8_t configured_view_count = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (sender_thread_running_.load() || network_thread_running_.load()) {
                return true;
            }

            if (renderer_ != kUnityGfxRendererD3D11 || unity_device_ == nullptr || unity_context_ == nullptr) {
                std::cerr << "UnitySender_Start failed: Unity D3D11 device is not ready.\n";
                return false;
            }

            should_auto_learn_target = UsesAutoTargetHostLocked();
        }

        {
            std::lock_guard<std::mutex> texture_lock(texture_mutex_);
            configured_view_count = ComputeConfiguredViewCountLocked();
            if (configured_view_count == 0) {
                std::cerr << "UnitySender_Start failed: no Unity texture handles have been provided.\n";
                return false;
            }

            if (!EnsureAllActiveSourceTexturesLocked()) {
                return false;
            }

            const std::uint32_t expected_width = view_textures_[0].source_width;
            const std::uint32_t expected_height = view_textures_[0].source_height;
            for (std::size_t view_index = 1; view_index < configured_view_count; ++view_index) {
                const auto& view_state = view_textures_[view_index];
                if (view_state.source_width != expected_width || view_state.source_height != expected_height) {
                    std::cerr << "UnitySender_Start failed: stereo textures must share one size. view0="
                              << expected_width << "x" << expected_height
                              << " view" << view_index << "=" << view_state.source_width << "x"
                              << view_state.source_height << "\n";
                    return false;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!winsock_started_) {
                WSADATA wsa_data{};
                if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
                    std::cerr << "UnitySender_Start failed: WSAStartup failed.\n";
                    return false;
                }
                winsock_started_ = true;
            }

            stop_requested_.store(false);
            configured_view_count_ = configured_view_count;
            next_packet_frame_id_.store(0);
            last_pose_ = LatestPoseState{};
            last_pose_sender_ipv4_.clear();
            pending_control_state_ = vt::windows::ControlRequestState{};
            pending_control_generation_ = 0;
            last_consumed_control_generation_.fill(0);
        }

        {
            std::lock_guard<std::mutex> texture_lock(texture_mutex_);
            copied_frame_pair_id_ = 0;
            copied_frame_ready_.store(false);
            last_consumed_copied_pair_id_.fill(0);
        }

        if (!StartNetworkThread()) {
            Stop();
            return false;
        }

        if (should_auto_learn_target) {
            std::string learned_host;
            if (!WaitForPoseSenderIpv4(std::chrono::milliseconds(1500), &learned_host)) {
                std::cerr << "UnitySender_Start deferred: target host is set to auto and no pose source was observed.\n";
                Stop();
                return false;
            }

            std::lock_guard<std::mutex> lock(state_mutex_);
            runtime_config_.host = learned_host;
            std::cout << "unity_sender_plugin learned target host from pose source: " << learned_host << "\n";
        }

        vt::windows::SenderRuntimeConfig sender_config{};
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            sender_config = runtime_config_;
        }

        for (std::size_t view_index = 0; view_index < configured_view_count; ++view_index) {
            sender_view_thread_running_[view_index].store(true);
            sender_threads_[view_index] = std::thread([this, sender_config, view_index]() {
                UnityTextureContentSource source(this, static_cast<std::uint8_t>(view_index));
                vt::windows::SenderRunOptions options{};
                options.stop_requested = &stop_requested_;
                options.shared_packet_frame_id = &next_packet_frame_id_;
                vt::windows::RunNvencVideoSender(sender_config, &source, &options);
                sender_view_thread_running_[view_index].store(false);
                UpdateSenderThreadRunningFlag();
            });
        }
        UpdateSenderThreadRunningFlag();

        return true;
    }

    void Stop() {
        stop_requested_.store(true);

        for (auto& sender_thread : sender_threads_) {
            if (sender_thread.joinable()) {
                sender_thread.join();
            }
        }
        for (auto& sender_running : sender_view_thread_running_) {
            sender_running.store(false);
        }
        sender_thread_running_.store(false);

        network_running_.store(false);
        if (network_thread_.joinable()) {
            network_thread_.join();
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        if (pose_socket_ != INVALID_SOCKET) {
            closesocket(pose_socket_);
            pose_socket_ = INVALID_SOCKET;
        }
        if (winsock_started_) {
            WSACleanup();
            winsock_started_ = false;
        }
        configured_view_count_ = 0;
        pending_control_state_ = vt::windows::ControlRequestState{};
        pending_control_generation_ = 0;
        last_consumed_control_generation_.fill(0);
        last_pose_sender_ipv4_.clear();
        pose_socket_ = INVALID_SOCKET;
        UpdateSenderThreadRunningFlag();
        {
            std::lock_guard<std::mutex> texture_lock(texture_mutex_);
            active_encode_width_ = 0;
            active_encode_height_ = 0;
            copied_frame_pair_id_ = 0;
            copied_frame_ready_.store(false);
            last_consumed_copied_pair_id_.fill(0);
            render_thread_copy_count_ = 0;
        }
    }

    bool IsRunning() const {
        return sender_thread_running_.load();
    }

    bool GetLatestPose(UnitySenderPose* out_pose) const {
        if (out_pose == nullptr) {
            return false;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        out_pose->position_m[0] = last_pose_.pose.position_m.x;
        out_pose->position_m[1] = last_pose_.pose.position_m.y;
        out_pose->position_m[2] = last_pose_.pose.position_m.z;
        out_pose->orientation_xyzw[0] = last_pose_.pose.orientation.x;
        out_pose->orientation_xyzw[1] = last_pose_.pose.orientation.y;
        out_pose->orientation_xyzw[2] = last_pose_.pose.orientation.z;
        out_pose->orientation_xyzw[3] = last_pose_.pose.orientation.w;
        out_pose->tracking_flags = last_pose_.pose.tracking_flags;
        out_pose->sequence = last_pose_.sequence;
        out_pose->timestamp_us = last_pose_.timestamp_us;
        out_pose->packets_received = last_pose_.packets_received;
        out_pose->sequence_gaps = last_pose_.sequence_gaps;
        out_pose->has_pose = last_pose_.has_pose ? 1 : 0;
        return last_pose_.has_pose;
    }

    bool GetStats(UnitySenderStats* out_stats) const {
        if (out_stats == nullptr) {
            return false;
        }

        std::lock_guard<std::mutex> state_lock(state_mutex_);
        std::lock_guard<std::mutex> texture_lock(texture_mutex_);

        out_stats->unity_device_ready = unity_device_ != nullptr ? 1 : 0;
        int source_texture_ready = configured_view_count_ > 0 ? 1 : 0;
        for (std::size_t view_index = 0; view_index < configured_view_count_; ++view_index) {
            if (view_textures_[view_index].source_texture == nullptr ||
                view_textures_[view_index].copied_texture == nullptr) {
                source_texture_ready = 0;
                break;
            }
        }
        out_stats->source_texture_ready = source_texture_ready;
        out_stats->copied_frame_ready = copied_frame_ready_.load() ? 1 : 0;
        out_stats->network_thread_running = network_thread_running_.load() ? 1 : 0;
        out_stats->sender_thread_running = sender_thread_running_.load() ? 1 : 0;
        out_stats->source_width = view_textures_[0].source_width;
        out_stats->source_height = view_textures_[0].source_height;
        out_stats->render_thread_copy_count = render_thread_copy_count_;
        out_stats->pose_packets_received = last_pose_.packets_received;
        out_stats->control_packets_received = pending_control_state_.packets_received;
        out_stats->last_pose_sequence = last_pose_.sequence;
        out_stats->configured_view_count = configured_view_count_;
        out_stats->latest_frame_pair_id = static_cast<std::uint32_t>(copied_frame_pair_id_);
        return true;
    }

    bool GetLastPoseSenderIpv4(char* out_buffer, std::size_t buffer_size) const {
        if (out_buffer == nullptr || buffer_size == 0) {
            return false;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        if (last_pose_sender_ipv4_.empty()) {
            out_buffer[0] = '\0';
            return false;
        }

        const auto copy_length = std::min(buffer_size - 1, last_pose_sender_ipv4_.size());
        std::memcpy(out_buffer, last_pose_sender_ipv4_.data(), copy_length);
        out_buffer[copy_length] = '\0';
        return true;
    }

    void OnRenderEvent(int event_id) {
        if (event_id != reserved_event_base_ + kRenderEventCopyTexture) {
            return;
        }

        std::lock_guard<std::mutex> lock(texture_mutex_);
        const auto configured_view_count = ComputeConfiguredViewCountLocked();
        if (configured_view_count == 0 || !EnsureAllActiveSourceTexturesLocked()) {
            return;
        }
        if (unity_context_ == nullptr) {
            return;
        }
        if (copied_frame_pair_id_ != 0 &&
            !HaveAllActiveViewsConsumedCopiedPairLocked(copied_frame_pair_id_, configured_view_count)) {
            return;
        }

        ImmediateContextGuard context_guard(unity_multithread_.Get());
        for (std::size_t view_index = 0; view_index < configured_view_count; ++view_index) {
            if (view_textures_[view_index].source_texture == nullptr ||
                view_textures_[view_index].copied_texture == nullptr) {
                return;
            }
            unity_context_->CopyResource(view_textures_[view_index].copied_texture.Get(),
                                         view_textures_[view_index].source_texture.Get());
        }
        copied_frame_pair_id_ += 1;
        copied_frame_ready_.store(true);
        render_thread_copy_count_ += 1;
    }

    int GetCopyTextureEventId() const {
        return reserved_event_base_ >= 0 ? reserved_event_base_ + kRenderEventCopyTexture : -1;
    }

    bool PrepareForSenderInitialize(std::uint8_t view_id,
                                    std::uint16_t* inout_width,
                                    std::uint16_t* inout_height) {
        std::lock_guard<std::mutex> lock(texture_mutex_);
        if (view_id >= kMaxStereoViews || !EnsureAllActiveSourceTexturesLocked()) {
            return false;
        }

        const auto& view_state = view_textures_[view_id];
        if (view_state.source_width == 0 || view_state.source_height == 0) {
            std::cerr << "Unity sender source texture dimensions are unavailable.\n";
            return false;
        }

        if (inout_width == nullptr || inout_height == nullptr) {
            return false;
        }

        if (*inout_width == 0) {
            *inout_width = static_cast<std::uint16_t>(view_state.source_width);
        }
        if (*inout_height == 0) {
            *inout_height = static_cast<std::uint16_t>(view_state.source_height);
        }

        if (*inout_width != view_state.source_width || *inout_height != view_state.source_height) {
            std::cerr << "Unity sender currently requires encode size to match the Unity RenderTexture size. "
                      << "encode=" << *inout_width << "x" << *inout_height
                      << " source=" << view_state.source_width << "x" << view_state.source_height
                      << "\n";
            return false;
        }

        if (active_encode_width_ == 0 && active_encode_height_ == 0) {
            active_encode_width_ = *inout_width;
            active_encode_height_ = *inout_height;
        } else if (active_encode_width_ != *inout_width || active_encode_height_ != *inout_height) {
            std::cerr << "Unity sender active encode size mismatch across views. active="
                      << active_encode_width_ << "x" << active_encode_height_
                      << " requested=" << *inout_width << "x" << *inout_height
                      << "\n";
            return false;
        }

        return true;
    }

    bool AcquireCopiedTexture(std::uint8_t view_id,
                              std::uint32_t* inout_last_pair_id,
                              ID3D11Texture2D** out_texture) {
        if (out_texture == nullptr || inout_last_pair_id == nullptr) {
            return false;
        }

        if (!copied_frame_ready_.load()) {
            *out_texture = nullptr;
            return false;
        }

        std::lock_guard<std::mutex> lock(texture_mutex_);
        if (view_id >= kMaxStereoViews) {
            *out_texture = nullptr;
            return false;
        }

        const auto& view_state = view_textures_[view_id];
        if (view_state.copied_texture == nullptr || copied_frame_pair_id_ == 0 ||
            copied_frame_pair_id_ == *inout_last_pair_id) {
            *out_texture = nullptr;
            return false;
        }

        *out_texture = view_state.copied_texture.Get();
        *inout_last_pair_id = static_cast<std::uint32_t>(copied_frame_pair_id_);
        last_consumed_copied_pair_id_[view_id] = copied_frame_pair_id_;
        return true;
    }

    void BeforeEncodeCopy() {
        texture_mutex_.lock();
        if (unity_multithread_ != nullptr) {
            unity_multithread_->Enter();
        }
    }

    void AfterEncodeCopy() {
        if (unity_multithread_ != nullptr) {
            unity_multithread_->Leave();
        }
        texture_mutex_.unlock();
    }

    ID3D11Device* unity_device() const noexcept {
        return unity_device_.Get();
    }

    ID3D11DeviceContext* unity_context() const noexcept {
        return unity_context_.Get();
    }

    std::string StartupDetails() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return "source=unity_render_texture pose_port=" + std::to_string(pose_port_);
    }

    std::string StartupDetails(std::uint8_t view_id) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return "view=" + std::to_string(view_id) + "/" + std::to_string(std::max<int>(configured_view_count_, 1)) +
               " source=unity_render_texture pose_port=" + std::to_string(pose_port_);
    }

    std::string FrameLogSuffix(std::uint8_t view_id) const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return "view=" + std::to_string(view_id) +
               " pose_packets=" + std::to_string(last_pose_.packets_received) +
               (last_pose_.has_pose ? " pose=active" : " pose=default");
    }

    bool ConsumePendingControl(std::uint8_t view_id,
                               std::uint64_t* inout_generation,
                               vt::windows::ControlRequestState* control_state) {
        if (control_state == nullptr || inout_generation == nullptr || view_id >= kMaxStereoViews) {
            return false;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        if (pending_control_generation_ == 0 || *inout_generation == pending_control_generation_) {
            return true;
        }

        control_state->request_keyframe =
            control_state->request_keyframe || pending_control_state_.request_keyframe;
        control_state->request_codec_config =
            control_state->request_codec_config || pending_control_state_.request_codec_config;
        if (pending_control_state_.request_keyframe || pending_control_state_.request_codec_config) {
            control_state->last_related_frame_id = pending_control_state_.last_related_frame_id;
        }
        *inout_generation = pending_control_generation_;
        last_consumed_control_generation_[view_id] = pending_control_generation_;
        if (HaveAllActiveViewsConsumedControlLocked(pending_control_generation_)) {
            pending_control_state_.request_keyframe = false;
            pending_control_state_.request_codec_config = false;
        }
        return true;
    }

    std::uint8_t configured_view_count() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return configured_view_count_;
    }

    vt::proto::VideoStereoFrameMetadata MakeStereoMetadata(std::uint8_t view_id,
                                                           std::uint32_t frame_pair_id) const noexcept {
        std::uint8_t configured_view_count = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            configured_view_count = configured_view_count_;
        }

        if (configured_view_count <= 1) {
            return vt::proto::MakeMonoVideoStereoFrameMetadata(frame_pair_id);
        }

        vt::proto::VideoStereoFrameMetadata metadata{};
        metadata.view_id = view_id < configured_view_count ? view_id : 0;
        metadata.view_count = configured_view_count;
        metadata.layout = static_cast<std::uint16_t>(vt::proto::VideoStereoLayout::ProjectionViews);
        metadata.frame_pair_id = frame_pair_id;
        return metadata;
    }

private:
    bool UsesAutoTargetHostLocked() const {
        return runtime_config_.host.empty() || _stricmp(runtime_config_.host.c_str(), "auto") == 0;
    }

    bool WaitForPoseSenderIpv4(std::chrono::milliseconds timeout, std::string* out_host) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline && !stop_requested_.load()) {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!last_pose_sender_ipv4_.empty()) {
                    if (out_host != nullptr) {
                        *out_host = last_pose_sender_ipv4_;
                    }
                    return true;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        return false;
    }

    bool StartNetworkThread() {
        if (network_thread_running_.load()) {
            return true;
        }

        SOCKET socket = INVALID_SOCKET;
        if (!vt::windows::OpenNonBlockingUdpSocket(pose_port_, "unity_sender_pose", &socket)) {
            return false;
        }

        // Start() already owns state_mutex_ while it performs the startup sequence.
        // Re-locking it here would deadlock before the sender thread can launch.
        pose_socket_ = socket;

        network_running_.store(true);
        network_thread_running_.store(true);
        network_thread_ = std::thread([this]() {
            while (network_running_.load() && !stop_requested_.load()) {
                bool received_packet = false;

                while (true) {
                    std::uint8_t buffer[1500]{};
                    sockaddr_in from{};
                    int from_len = sizeof(from);
                    const int bytes = recvfrom(pose_socket_,
                                               reinterpret_cast<char*>(buffer),
                                               static_cast<int>(sizeof(buffer)),
                                               0,
                                               reinterpret_cast<sockaddr*>(&from),
                                               &from_len);
                    if (bytes == SOCKET_ERROR) {
                        const int error = WSAGetLastError();
                        if (error != WSAEWOULDBLOCK) {
                            std::cerr << "unity_sender_plugin recvfrom failed: " << error << "\n";
                        }
                        break;
                    }

                    received_packet = true;
                    HandleIncomingPacket(buffer, static_cast<std::size_t>(bytes), from);
                }

                if (!received_packet) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }

            network_thread_running_.store(false);
        });

        return true;
    }

    void HandleIncomingPacket(const std::uint8_t* data, std::size_t bytes, const sockaddr_in& from) {
        if (data == nullptr || bytes < sizeof(vt::proto::PacketHeader)) {
            return;
        }

        vt::proto::PacketHeader header{};
        std::memcpy(&header, data, sizeof(header));
        if (!vt::proto::IsValidHeader(header)) {
            return;
        }

        if (header.type == static_cast<std::uint16_t>(vt::proto::PacketType::Control)) {
            if (header.payload_size != sizeof(vt::proto::ControlPayload) ||
                bytes < sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::ControlPayload)) {
                return;
            }

            vt::proto::ControlPayload payload{};
            std::memcpy(&payload, data + sizeof(header), sizeof(payload));
            std::lock_guard<std::mutex> lock(state_mutex_);
            vt::windows::HandleControlPayload(payload, &pending_control_state_);
            if (pending_control_state_.request_keyframe || pending_control_state_.request_codec_config) {
                pending_control_generation_ += 1;
            }
            UpdateLastPoseSenderIpv4Locked(from);
            return;
        }

        if (header.type != static_cast<std::uint16_t>(vt::proto::PacketType::Pose) ||
            header.payload_size != sizeof(vt::proto::PosePayload) ||
            bytes < sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::PosePayload)) {
            return;
        }

        vt::proto::PosePayload pose{};
        std::memcpy(&pose, data + sizeof(header), sizeof(pose));
        NormalizeQuaternion(&pose.orientation);

        std::lock_guard<std::mutex> lock(state_mutex_);
        if (last_pose_.has_pose && header.sequence > last_pose_.sequence + 1) {
            last_pose_.sequence_gaps += static_cast<std::uint64_t>(header.sequence - last_pose_.sequence - 1);
        }
        last_pose_.pose = pose;
        last_pose_.sequence = header.sequence;
        last_pose_.timestamp_us = header.timestamp_us;
        last_pose_.has_pose = true;
        last_pose_.packets_received += 1;
        UpdateLastPoseSenderIpv4Locked(from);
    }

    void UpdateLastPoseSenderIpv4Locked(const sockaddr_in& from) {
        std::array<char, INET_ADDRSTRLEN> host_buffer{};
        if (inet_ntop(AF_INET, &from.sin_addr, host_buffer.data(), static_cast<DWORD>(host_buffer.size())) != nullptr) {
            last_pose_sender_ipv4_ = host_buffer.data();
        }
    }

    std::uint8_t ComputeConfiguredViewCountLocked() const {
        const bool has_view_0 =
            view_textures_[0].pending_texture_handle != nullptr || view_textures_[0].current_texture_handle != nullptr;
        const bool has_view_1 =
            view_textures_[1].pending_texture_handle != nullptr || view_textures_[1].current_texture_handle != nullptr;
        if (!has_view_0) {
            return 0;
        }
        return has_view_1 ? 2 : 1;
    }

    bool EnsureAllActiveSourceTexturesLocked() {
        const auto configured_view_count = ComputeConfiguredViewCountLocked();
        if (configured_view_count == 0) {
            return false;
        }

        std::uint32_t expected_width = 0;
        std::uint32_t expected_height = 0;
        for (std::size_t view_index = 0; view_index < configured_view_count; ++view_index) {
            if (!EnsureSourceTextureLocked(view_index)) {
                return false;
            }

            const auto& view_state = view_textures_[view_index];
            if (view_state.source_width == 0 || view_state.source_height == 0) {
                return false;
            }

            if (expected_width == 0 && expected_height == 0) {
                expected_width = view_state.source_width;
                expected_height = view_state.source_height;
                continue;
            }

            if (view_state.source_width != expected_width || view_state.source_height != expected_height) {
                std::cerr << "Unity sender stereo view size mismatch. view0=" << expected_width << "x"
                          << expected_height << " view" << view_index << "=" << view_state.source_width << "x"
                          << view_state.source_height << "\n";
                return false;
            }
        }

        return true;
    }

    bool EnsureSourceTextureLocked(std::size_t view_index) {
        auto& view_state = view_textures_[view_index];
        void* texture_handle =
            view_state.pending_texture_handle != nullptr ? view_state.pending_texture_handle : view_state.current_texture_handle;
        if (texture_handle == nullptr) {
            return false;
        }

        if (texture_handle == view_state.current_texture_handle && view_state.source_texture != nullptr &&
            view_state.copied_texture != nullptr) {
            return true;
        }

        auto* resource = reinterpret_cast<ID3D11Resource*>(texture_handle);
        if (resource == nullptr) {
            return false;
        }

        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) || texture == nullptr) {
            std::cerr << "UnitySender_SetTexture received a non-ID3D11Texture2D resource.\n";
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width == 0 || desc.Height == 0) {
            return false;
        }

        const bool should_log_format = !view_state.source_format_logged || view_state.source_format != desc.Format;
        view_state.source_format = desc.Format;
        view_state.source_format_logged = true;
        if (should_log_format) {
            std::cout << "unity_sender_plugin source texture format view=" << view_index << " "
                      << DxgiFormatName(desc.Format)
                      << " (" << static_cast<unsigned>(desc.Format) << ")"
                      << " size=" << desc.Width << "x" << desc.Height
                      << std::endl;
        }

        if (!IsNvencArgbCopyCompatibleFormat(desc.Format)) {
            if (!view_state.source_format_error_logged) {
                std::cerr << "Unity sender source texture format is not compatible with the current NVENC ARGB path. "
                          << "view=" << view_index << " "
                          << "source=" << DxgiFormatName(desc.Format)
                          << " (" << static_cast<unsigned>(desc.Format) << ")"
                          << " required=DXGI_FORMAT_B8G8R8A8_UNORM. "
                          << "Use a BGRA32 Linear Unity RenderTexture or add a shader conversion path."
                          << std::endl;
                view_state.source_format_error_logged = true;
            }
            return false;
        }

        if (sender_thread_running_.load() && active_encode_width_ != 0 && active_encode_height_ != 0 &&
            (desc.Width != active_encode_width_ || desc.Height != active_encode_height_)) {
            std::cerr << "UnitySender_SetTexture attempted to switch to a different size while streaming. "
                      << "restart the sender after recreating the RenderTexture. "
                      << "new=" << desc.Width << "x" << desc.Height
                      << " active=" << active_encode_width_ << "x" << active_encode_height_
                      << "\n";
            return false;
        }

        D3D11_TEXTURE2D_DESC copied_desc = desc;
        copied_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        copied_desc.BindFlags = 0;
        copied_desc.MiscFlags = 0;
        copied_desc.Usage = D3D11_USAGE_DEFAULT;
        copied_desc.CPUAccessFlags = 0;

        ComPtr<ID3D11Texture2D> copied_texture;
        if (unity_device_ == nullptr ||
            FAILED(unity_device_->CreateTexture2D(&copied_desc, nullptr, &copied_texture)) ||
            copied_texture == nullptr) {
            std::cerr << "unity_sender_plugin failed to create copied texture.\n";
            return false;
        }

        view_state.source_texture = texture;
        view_state.copied_texture = copied_texture;
        view_state.current_texture_handle = texture_handle;
        view_state.pending_texture_handle = nullptr;
        view_state.source_width = desc.Width;
        view_state.source_height = desc.Height;
        copied_frame_ready_.store(false);
        return true;
    }

    bool HaveAllActiveViewsConsumedControlLocked(std::uint64_t generation) const {
        if (configured_view_count_ == 0) {
            return true;
        }

        for (std::size_t view_index = 0; view_index < configured_view_count_; ++view_index) {
            if (last_consumed_control_generation_[view_index] < generation) {
                return false;
            }
        }
        return true;
    }

    bool HaveAllActiveViewsConsumedCopiedPairLocked(std::uint64_t pair_id, std::uint8_t active_view_count) const {
        if (pair_id == 0 || active_view_count == 0) {
            return true;
        }

        for (std::size_t view_index = 0; view_index < active_view_count; ++view_index) {
            if (last_consumed_copied_pair_id_[view_index] < pair_id) {
                return false;
            }
        }
        return true;
    }

    void UpdateSenderThreadRunningFlag() {
        bool any_running = false;
        for (const auto& sender_running : sender_view_thread_running_) {
            if (sender_running.load()) {
                any_running = true;
                break;
            }
        }
        sender_thread_running_.store(any_running);
    }

    void ClearViewTextureStateLocked(std::size_t view_index) {
        view_textures_[view_index] = ViewTextureState{};
        copied_frame_ready_.store(false);
        copied_frame_pair_id_ = 0;
        last_consumed_copied_pair_id_.fill(0);
    }

    mutable std::mutex state_mutex_;
    mutable std::mutex texture_mutex_;

    IUnityInterfaces* unity_interfaces_ = nullptr;
    IUnityGraphics* unity_graphics_ = nullptr;
    IUnityGraphicsD3D11* unity_graphics_d3d11_ = nullptr;
    UnityGfxRenderer renderer_ = kUnityGfxRendererNull;
    int reserved_event_base_ = -1;

    ComPtr<ID3D11Device> unity_device_;
    ComPtr<ID3D11DeviceContext> unity_context_;
    ComPtr<ID3D11Multithread> unity_multithread_;

    std::array<ViewTextureState, kMaxStereoViews> view_textures_{};
    std::uint64_t render_thread_copy_count_ = 0;
    std::atomic<bool> copied_frame_ready_{false};
    std::uint64_t copied_frame_pair_id_ = 0;
    std::uint32_t active_encode_width_ = 0;
    std::uint32_t active_encode_height_ = 0;

    vt::windows::SenderRuntimeConfig runtime_config_{};
    std::uint16_t pose_port_ = 25672;
    std::uint8_t configured_view_count_ = 0;

    LatestPoseState last_pose_{};
    std::string last_pose_sender_ipv4_{};
    vt::windows::ControlRequestState pending_control_state_{};
    std::uint64_t pending_control_generation_ = 0;
    std::array<std::uint64_t, kMaxStereoViews> last_consumed_control_generation_{};
    std::array<std::uint64_t, kMaxStereoViews> last_consumed_copied_pair_id_{};

    SOCKET pose_socket_ = INVALID_SOCKET;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> network_running_{false};
    std::atomic<bool> network_thread_running_{false};
    std::atomic<bool> sender_thread_running_{false};
    std::array<std::atomic<bool>, kMaxStereoViews> sender_view_thread_running_{};
    std::atomic<std::uint32_t> next_packet_frame_id_{0};
    std::thread network_thread_;
    std::array<std::thread, kMaxStereoViews> sender_threads_{};
    bool winsock_started_ = false;
};

UnitySenderPluginState& GetPluginState() {
    static UnitySenderPluginState state;
    return state;
}

void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType event_type) {
    GetPluginState().OnGraphicsDeviceEvent(event_type);
}

void UNITY_INTERFACE_API OnRenderEvent(int event_id) {
    GetPluginState().OnRenderEvent(event_id);
}

UnityTextureContentSource::UnityTextureContentSource(UnitySenderPluginState* owner, std::uint8_t view_id)
    : owner_(owner),
      view_id_(view_id),
      frame_log_label_("unity_view" + std::to_string(view_id)) {}

bool UnityTextureContentSource::Initialize(std::uint16_t* inout_width, std::uint16_t* inout_height) {
    return owner_ != nullptr && owner_->PrepareForSenderInitialize(view_id_, inout_width, inout_height);
}

bool UnityTextureContentSource::UpdateAndRender(const vt::windows::FrameContext& frame_context,
                                                vt::windows::ControlRequestState* control_state,
                                                ID3D11Texture2D** out_texture) {
    if (owner_ == nullptr || out_texture == nullptr) {
        return false;
    }

    owner_->ConsumePendingControl(view_id_, &last_control_generation_, control_state);
    if (!owner_->AcquireCopiedTexture(view_id_, &last_acquired_pair_id_, out_texture)) {
        return false;
    }

    if (frame_context.frame_index <= 3) {
        frame_log_label_ = "unity_view" + std::to_string(view_id_);
    }
    return *out_texture != nullptr;
}

ID3D11Device* UnityTextureContentSource::device() const noexcept {
    return owner_ != nullptr ? owner_->unity_device() : nullptr;
}

ID3D11DeviceContext* UnityTextureContentSource::context() const noexcept {
    return owner_ != nullptr ? owner_->unity_context() : nullptr;
}

std::string UnityTextureContentSource::StartupDetails() const {
    return owner_ != nullptr ? owner_->StartupDetails(view_id_) : std::string{};
}

std::string UnityTextureContentSource::FrameLogSuffix() const {
    return owner_ != nullptr ? owner_->FrameLogSuffix(view_id_) : std::string{};
}

vt::proto::VideoStereoFrameMetadata UnityTextureContentSource::EncodedCodecConfigStereoMetadata() const noexcept {
    return owner_ != nullptr ? owner_->MakeStereoMetadata(view_id_, 0) : vt::proto::MakeMonoVideoStereoFrameMetadata();
}

vt::proto::VideoStereoFrameMetadata UnityTextureContentSource::EncodedFrameStereoMetadata(
    const vt::windows::FrameContext&) const noexcept {
    return owner_ != nullptr ? owner_->MakeStereoMetadata(view_id_, last_acquired_pair_id_)
                             : vt::proto::MakeMonoVideoStereoFrameMetadata();
}

void UnityTextureContentSource::BeforeEncodeCopy() {
    if (owner_ != nullptr) {
        owner_->BeforeEncodeCopy();
    }
}

void UnityTextureContentSource::AfterEncodeCopy() {
    if (owner_ != nullptr) {
        owner_->AfterEncodeCopy();
    }
}

void UnityTextureContentSource::Shutdown() noexcept {}

}  // namespace

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unity_interfaces) {
    auto& state = GetPluginState();
    state.SetInterfaces(unity_interfaces);
    if (auto* graphics = unity_interfaces != nullptr ? unity_interfaces->Get<IUnityGraphics>() : nullptr) {
        graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
    }
    OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
}

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload() {
    auto& state = GetPluginState();
    state.UnregisterDeviceCallback();
    state.Stop();
}

bool UnitySender_Configure(const char* target_host,
                           std::uint16_t video_port,
                           std::uint16_t pose_port,
                           int fps,
                           std::uint32_t bitrate,
                           std::uint16_t width,
                           std::uint16_t height) {
    return GetPluginState().Configure(target_host, video_port, pose_port, fps, bitrate, width, height);
}

void UnitySender_SetTexture(void* texture_handle) {
    GetPluginState().SetTextureHandle(texture_handle);
}

void UnitySender_SetTextureForView(int view_id, void* texture_handle) {
    GetPluginState().SetTextureHandleForView(view_id, texture_handle);
}

bool UnitySender_Start() {
    return GetPluginState().Start();
}

void UnitySender_Stop() {
    GetPluginState().Stop();
}

bool UnitySender_IsRunning() {
    return GetPluginState().IsRunning();
}

bool UnitySender_GetLatestPose(UnitySenderPose* out_pose) {
    return GetPluginState().GetLatestPose(out_pose);
}

bool UnitySender_GetStats(UnitySenderStats* out_stats) {
    return GetPluginState().GetStats(out_stats);
}

bool UnitySender_GetLastPoseSenderIpv4(char* out_buffer, std::size_t buffer_size) {
    return GetPluginState().GetLastPoseSenderIpv4(out_buffer, buffer_size);
}

int UnitySender_GetCopyTextureEventId() {
    return GetPluginState().GetCopyTextureEventId();
}

void* UnitySender_GetRenderEventFunc() {
    return reinterpret_cast<void*>(OnRenderEvent);
}
