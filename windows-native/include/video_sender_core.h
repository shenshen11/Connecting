#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <cstdint>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace vt::proto {
struct ControlPayload;
}

namespace vt::windows {

struct SenderRuntimeConfig final {
    std::string host = "127.0.0.1";
    std::uint16_t video_port = 25674;
    std::uint16_t width = 1280;
    std::uint16_t height = 720;
    int fps = 30;
    std::uint32_t bitrate = 4'000'000;
};

struct ControlRequestState final {
    bool request_keyframe = false;
    bool request_codec_config = false;
    std::uint32_t last_related_frame_id = 0;
    std::uint64_t packets_received = 0;

    void ClearPending() noexcept {
        request_keyframe = false;
        request_codec_config = false;
    }
};

struct FrameContext final {
    std::uint32_t frame_index = 0;
    double time_seconds = 0.0;
};

class IVideoContentSource {
public:
    virtual ~IVideoContentSource() = default;

    virtual bool Initialize(std::uint16_t* inout_width, std::uint16_t* inout_height) = 0;
    virtual bool UpdateAndRender(const FrameContext& frame_context,
                                 ControlRequestState* control_state,
                                 ID3D11Texture2D** out_texture) = 0;

    virtual ID3D11Device* device() const noexcept = 0;
    virtual ID3D11DeviceContext* context() const noexcept = 0;

    virtual const char* SenderName() const noexcept = 0;
    virtual const char* FrameLogLabel() const noexcept = 0;
    virtual std::uint32_t FrameLogInterval() const noexcept = 0;
    virtual std::string StartupDetails() const = 0;

    virtual std::string FrameLogSuffix() const { return {}; }
    virtual void BeforeEncodeCopy() {}
    virtual void AfterEncodeCopy() {}
    virtual void Shutdown() noexcept {}
};

struct SenderRunOptions final {
    std::atomic<bool>* stop_requested = nullptr;
};

bool OpenNonBlockingUdpSocket(std::uint16_t port, const char* label, SOCKET* out_socket);
void HandleControlPayload(const vt::proto::ControlPayload& payload, ControlRequestState* state);
void PollControlPackets(SOCKET sock, ControlRequestState* state);

int RunNvencVideoSender(const SenderRuntimeConfig& config,
                        IVideoContentSource* source,
                        const SenderRunOptions* options = nullptr);

}  // namespace vt::windows
