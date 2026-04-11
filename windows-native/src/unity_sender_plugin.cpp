#include "unity_sender_plugin.h"

#include "control_protocol.h"
#include "packet_defs.h"
#include "pose_protocol.h"
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

class UnitySenderPluginState;
void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType event_type);
void UNITY_INTERFACE_API OnRenderEvent(int event_id);

class UnityTextureContentSource final : public vt::windows::IVideoContentSource {
public:
    explicit UnityTextureContentSource(UnitySenderPluginState* owner) : owner_(owner) {}

    bool Initialize(std::uint16_t* inout_width, std::uint16_t* inout_height) override;
    bool UpdateAndRender(const vt::windows::FrameContext& frame_context,
                         vt::windows::ControlRequestState* control_state,
                         ID3D11Texture2D** out_texture) override;

    ID3D11Device* device() const noexcept override;
    ID3D11DeviceContext* context() const noexcept override;
    const char* SenderName() const noexcept override { return "unity_sender_plugin"; }
    const char* FrameLogLabel() const noexcept override { return "unity"; }
    std::uint32_t FrameLogInterval() const noexcept override { return 60; }
    std::string StartupDetails() const override;
    std::string FrameLogSuffix() const override;
    void BeforeEncodeCopy() override;
    void AfterEncodeCopy() override;

private:
    UnitySenderPluginState* owner_ = nullptr;
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

            std::lock_guard<std::mutex> lock(state_mutex_);
            renderer_ = kUnityGfxRendererNull;
            unity_context_.Reset();
            unity_multithread_.Reset();
            unity_device_.Reset();
            source_texture_.Reset();
            copied_texture_.Reset();
            pending_texture_handle_ = nullptr;
            current_texture_handle_ = nullptr;
            source_width_ = 0;
            source_height_ = 0;
            copied_frame_ready_ = false;
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
        std::lock_guard<std::mutex> lock(texture_mutex_);
        pending_texture_handle_ = texture_handle;
    }

    bool Start() {
        bool should_auto_learn_target = false;
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
            if (pending_texture_handle_ == nullptr && current_texture_handle_ == nullptr) {
                std::cerr << "UnitySender_Start failed: no Unity texture handle has been provided.\n";
                return false;
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
            copied_frame_ready_.store(false);
            last_pose_ = LatestPoseState{};
            last_pose_sender_ipv4_.clear();
            pending_control_state_ = vt::windows::ControlRequestState{};
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

        sender_thread_running_.store(true);
        sender_thread_ = std::thread([this, sender_config]() {
            UnityTextureContentSource source(this);
            vt::windows::SenderRunOptions options{};
            options.stop_requested = &stop_requested_;
            vt::windows::RunNvencVideoSender(sender_config, &source, &options);
            sender_thread_running_.store(false);
        });

        return true;
    }

    void Stop() {
        stop_requested_.store(true);

        if (sender_thread_.joinable()) {
            sender_thread_.join();
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
        out_stats->source_texture_ready = source_texture_ != nullptr ? 1 : 0;
        out_stats->copied_frame_ready = copied_frame_ready_.load() ? 1 : 0;
        out_stats->network_thread_running = network_thread_running_.load() ? 1 : 0;
        out_stats->sender_thread_running = sender_thread_running_.load() ? 1 : 0;
        out_stats->source_width = source_width_;
        out_stats->source_height = source_height_;
        out_stats->render_thread_copy_count = render_thread_copy_count_;
        out_stats->pose_packets_received = last_pose_.packets_received;
        out_stats->control_packets_received = pending_control_state_.packets_received;
        out_stats->last_pose_sequence = last_pose_.sequence;
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
        if (!EnsureSourceTexturesLocked()) {
            return;
        }
        if (unity_context_ == nullptr || source_texture_ == nullptr || copied_texture_ == nullptr) {
            return;
        }

        ImmediateContextGuard context_guard(unity_multithread_.Get());
        unity_context_->CopyResource(copied_texture_.Get(), source_texture_.Get());
        copied_frame_ready_.store(true);
        render_thread_copy_count_ += 1;
    }

    int GetCopyTextureEventId() const {
        return reserved_event_base_ >= 0 ? reserved_event_base_ + kRenderEventCopyTexture : -1;
    }

    bool PrepareForSenderInitialize(std::uint16_t* inout_width, std::uint16_t* inout_height) {
        std::lock_guard<std::mutex> lock(texture_mutex_);
        if (!EnsureSourceTexturesLocked()) {
            return false;
        }

        if (source_width_ == 0 || source_height_ == 0) {
            std::cerr << "Unity sender source texture dimensions are unavailable.\n";
            return false;
        }

        if (inout_width == nullptr || inout_height == nullptr) {
            return false;
        }

        if (*inout_width == 0) {
            *inout_width = static_cast<std::uint16_t>(source_width_);
        }
        if (*inout_height == 0) {
            *inout_height = static_cast<std::uint16_t>(source_height_);
        }

        if (*inout_width != source_width_ || *inout_height != source_height_) {
            std::cerr << "Unity sender currently requires encode size to match the Unity RenderTexture size. "
                      << "encode=" << *inout_width << "x" << *inout_height
                      << " source=" << source_width_ << "x" << source_height_
                      << "\n";
            return false;
        }

        return true;
    }

    bool AcquireCopiedTexture(ID3D11Texture2D** out_texture) {
        if (out_texture == nullptr) {
            return false;
        }

        if (!copied_frame_ready_.load()) {
            *out_texture = nullptr;
            return false;
        }

        std::lock_guard<std::mutex> lock(texture_mutex_);
        if (copied_texture_ == nullptr) {
            *out_texture = nullptr;
            return false;
        }

        *out_texture = copied_texture_.Get();
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

    std::string FrameLogSuffix() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return "pose_packets=" + std::to_string(last_pose_.packets_received) +
               (last_pose_.has_pose ? " pose=active" : " pose=default");
    }

    bool ConsumePendingControl(vt::windows::ControlRequestState* control_state) {
        if (control_state == nullptr) {
            return false;
        }

        std::lock_guard<std::mutex> lock(state_mutex_);
        control_state->request_keyframe = control_state->request_keyframe || pending_control_state_.request_keyframe;
        control_state->request_codec_config =
            control_state->request_codec_config || pending_control_state_.request_codec_config;
        if (pending_control_state_.request_keyframe || pending_control_state_.request_codec_config) {
            control_state->last_related_frame_id = pending_control_state_.last_related_frame_id;
        }
        pending_control_state_.request_keyframe = false;
        pending_control_state_.request_codec_config = false;
        return true;
    }

    void SetActiveEncodeSize(std::uint32_t width, std::uint32_t height) {
        std::lock_guard<std::mutex> lock(texture_mutex_);
        active_encode_width_ = width;
        active_encode_height_ = height;
    }

    void ResetActiveEncodeSize() {
        std::lock_guard<std::mutex> lock(texture_mutex_);
        active_encode_width_ = 0;
        active_encode_height_ = 0;
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

    bool EnsureSourceTexturesLocked() {
        void* texture_handle = pending_texture_handle_ != nullptr ? pending_texture_handle_ : current_texture_handle_;
        if (texture_handle == nullptr) {
            return false;
        }

        if (texture_handle == current_texture_handle_ && source_texture_ != nullptr && copied_texture_ != nullptr) {
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

        source_texture_ = texture;
        copied_texture_ = copied_texture;
        current_texture_handle_ = texture_handle;
        pending_texture_handle_ = nullptr;
        source_width_ = desc.Width;
        source_height_ = desc.Height;
        copied_frame_ready_.store(false);
        return true;
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

    void* pending_texture_handle_ = nullptr;
    void* current_texture_handle_ = nullptr;
    ComPtr<ID3D11Texture2D> source_texture_;
    ComPtr<ID3D11Texture2D> copied_texture_;
    std::uint32_t source_width_ = 0;
    std::uint32_t source_height_ = 0;
    std::uint64_t render_thread_copy_count_ = 0;
    std::atomic<bool> copied_frame_ready_{false};
    std::uint32_t active_encode_width_ = 0;
    std::uint32_t active_encode_height_ = 0;

    vt::windows::SenderRuntimeConfig runtime_config_{};
    std::uint16_t pose_port_ = 25672;

    LatestPoseState last_pose_{};
    std::string last_pose_sender_ipv4_{};
    vt::windows::ControlRequestState pending_control_state_{};

    SOCKET pose_socket_ = INVALID_SOCKET;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> network_running_{false};
    std::atomic<bool> network_thread_running_{false};
    std::atomic<bool> sender_thread_running_{false};
    std::thread network_thread_;
    std::thread sender_thread_;
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

bool UnityTextureContentSource::Initialize(std::uint16_t* inout_width, std::uint16_t* inout_height) {
    if (owner_ == nullptr || !owner_->PrepareForSenderInitialize(inout_width, inout_height)) {
        return false;
    }
    owner_->SetActiveEncodeSize(*inout_width, *inout_height);
    return true;
}

bool UnityTextureContentSource::UpdateAndRender(const vt::windows::FrameContext&,
                                                vt::windows::ControlRequestState* control_state,
                                                ID3D11Texture2D** out_texture) {
    if (owner_ == nullptr || out_texture == nullptr) {
        return false;
    }

    owner_->ConsumePendingControl(control_state);
    return owner_->AcquireCopiedTexture(out_texture);
}

ID3D11Device* UnityTextureContentSource::device() const noexcept {
    return owner_ != nullptr ? owner_->unity_device() : nullptr;
}

ID3D11DeviceContext* UnityTextureContentSource::context() const noexcept {
    return owner_ != nullptr ? owner_->unity_context() : nullptr;
}

std::string UnityTextureContentSource::StartupDetails() const {
    return owner_ != nullptr ? owner_->StartupDetails() : std::string{};
}

std::string UnityTextureContentSource::FrameLogSuffix() const {
    return owner_ != nullptr ? owner_->FrameLogSuffix() : std::string{};
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
