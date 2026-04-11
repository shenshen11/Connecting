#include "packet_defs.h"
#include "control_protocol.h"
#include "video_protocol.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "NvEncoder/NvEncoderD3D11.h"
#include "video_sender_core.h"

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t kChunkPayloadBytes = 1200;

struct SenderConfig final {
    std::string host = "127.0.0.1";
    std::uint16_t port = 25674;
    std::uint16_t control_port = 25672;
    int fps = 15;
    std::uint32_t bitrate = 6'000'000;
    int output_index = 0;
    std::uint16_t width = 1280;
    std::uint16_t height = 720;
};

struct ControlRequestState final {
    bool request_keyframe = false;
    bool request_codec_config = false;
    std::uint32_t last_related_frame_id = 0;
    std::uint64_t packets_received = 0;
};

struct CaptureOutputInfo final {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc{};
    int global_index = -1;
};

struct ScopedDuplicationFrame final {
    IDXGIOutputDuplication* duplication = nullptr;
    bool acquired = false;

    ~ScopedDuplicationFrame() {
        if (duplication != nullptr && acquired) {
            duplication->ReleaseFrame();
        }
    }
};

void SendEncodedFrame(SOCKET sock,
                      const sockaddr_in& target,
                      std::uint32_t frame_id,
                      std::uint16_t width,
                      std::uint16_t height,
                      std::uint16_t flags,
                      const std::vector<std::uint8_t>& payload) {
    const std::uint32_t frame_size = static_cast<std::uint32_t>(payload.size());
    const std::uint16_t chunk_count =
        static_cast<std::uint16_t>((frame_size + kChunkPayloadBytes - 1) / kChunkPayloadBytes);

    for (std::uint16_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
        const std::uint32_t chunk_offset = static_cast<std::uint32_t>(chunk_index * kChunkPayloadBytes);
        const std::uint16_t chunk_size = static_cast<std::uint16_t>(
            std::min<std::size_t>(kChunkPayloadBytes, payload.size() - chunk_offset));

        vt::proto::PacketHeader packet_header{};
        packet_header.type = static_cast<std::uint16_t>(vt::proto::PacketType::Video);
        packet_header.sequence = frame_id;
        packet_header.payload_size = sizeof(vt::proto::EncodedVideoChunkHeader) + chunk_size;
        packet_header.timestamp_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());

        vt::proto::EncodedVideoChunkHeader encoded_header{};
        encoded_header.frame_id = frame_id;
        encoded_header.width = width;
        encoded_header.height = height;
        encoded_header.codec = static_cast<std::uint16_t>(vt::proto::VideoCodec::H264AnnexB);
        encoded_header.flags = flags;
        encoded_header.frame_size = frame_size;
        encoded_header.chunk_offset = chunk_offset;
        encoded_header.chunk_size = chunk_size;
        encoded_header.chunk_index = chunk_index;
        encoded_header.chunk_count = chunk_count;

        std::vector<std::uint8_t> packet(sizeof(packet_header) + sizeof(encoded_header) + chunk_size);
        std::memcpy(packet.data(), &packet_header, sizeof(packet_header));
        std::memcpy(packet.data() + sizeof(packet_header), &encoded_header, sizeof(encoded_header));
        std::memcpy(packet.data() + sizeof(packet_header) + sizeof(encoded_header),
                    payload.data() + chunk_offset,
                    chunk_size);

        sendto(sock,
               reinterpret_cast<const char*>(packet.data()),
               static_cast<int>(packet.size()),
               0,
               reinterpret_cast<const sockaddr*>(&target),
               sizeof(target));
    }
}

bool EnumerateOutputs(std::vector<CaptureOutputInfo>* outputs) {
    outputs->clear();

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::cerr << "CreateDXGIFactory1 failed.\n";
        return false;
    }

    int global_index = 0;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = factory->EnumAdapters1(adapter_index, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            continue;
        }

        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            hr = adapter->EnumOutputs(output_index, &output);
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(hr)) {
                continue;
            }

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) {
                continue;
            }

            CaptureOutputInfo info{};
            info.adapter = adapter;
            info.output = output;
            info.desc = desc;
            info.global_index = global_index++;
            outputs->push_back(info);
        }
    }

    return !outputs->empty();
}

HRESULT CompileShader(const char* source, const char* entry, const char* target, ID3DBlob** blob) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> error_blob;
    const HRESULT hr = D3DCompile(source,
                                  std::strlen(source),
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  entry,
                                  target,
                                  flags,
                                  0,
                                  blob,
                                  &error_blob);
    if (FAILED(hr) && error_blob) {
        std::cerr << "D3DCompile failed: "
                  << static_cast<const char*>(error_blob->GetBufferPointer()) << "\n";
    }
    return hr;
}

bool OpenControlSocket(std::uint16_t port, SOCKET* out_socket) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "control socket() failed.\n";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "control bind() failed on port " << port << ".\n";
        closesocket(sock);
        return false;
    }

    u_long non_blocking = 1;
    if (ioctlsocket(sock, FIONBIO, &non_blocking) == SOCKET_ERROR) {
        std::cerr << "control ioctlsocket(FIONBIO) failed.\n";
        closesocket(sock);
        return false;
    }

    *out_socket = sock;
    return true;
}

void PollControlPackets(SOCKET sock, ControlRequestState* state) {
    while (true) {
        std::uint8_t buffer[1500]{};
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int bytes = recvfrom(sock,
                                   reinterpret_cast<char*>(buffer),
                                   static_cast<int>(sizeof(buffer)),
                                   0,
                                   reinterpret_cast<sockaddr*>(&from),
                                   &from_len);
        if (bytes == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                std::cerr << "recvfrom(control) failed: " << error << "\n";
            }
            break;
        }

        if (bytes < static_cast<int>(sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::ControlPayload))) {
            continue;
        }

        vt::proto::PacketHeader header{};
        std::memcpy(&header, buffer, sizeof(header));
        if (!vt::proto::IsValidHeader(header) ||
            header.type != static_cast<std::uint16_t>(vt::proto::PacketType::Control) ||
            header.payload_size != sizeof(vt::proto::ControlPayload)) {
            continue;
        }

        vt::proto::ControlPayload payload{};
        std::memcpy(&payload, buffer + sizeof(header), sizeof(payload));
        const auto message_type = static_cast<vt::proto::ControlMessageType>(payload.message_type);
        state->packets_received += 1;
        state->last_related_frame_id = payload.related_frame_id;

        if (message_type == vt::proto::ControlMessageType::RequestCodecConfig) {
            state->request_codec_config = true;
        } else if (message_type == vt::proto::ControlMessageType::RequestKeyframe) {
            state->request_keyframe = true;
        } else {
            continue;
        }

        if (state->packets_received <= 5 || (state->packets_received % 60) == 0) {
            std::cout << "control request type=" << vt::proto::ControlMessageTypeName(message_type)
                      << " relatedFrame=" << payload.related_frame_id
                      << " requestId=" << payload.request_id
                      << " flags=0x" << std::hex << payload.flags << std::dec
                      << std::endl;
        }
    }
}

class DesktopDuplicator final {
public:
    bool Initialize(int output_index, std::uint32_t* out_width, std::uint32_t* out_height) {
        std::vector<CaptureOutputInfo> outputs;
        if (!EnumerateOutputs(&outputs)) {
            std::cerr << "No desktop outputs found.\n";
            return false;
        }

        const CaptureOutputInfo* chosen = nullptr;
        for (const auto& info : outputs) {
            if (info.global_index == output_index) {
                chosen = &info;
                break;
            }
        }
        if (chosen == nullptr) {
            chosen = &outputs.front();
        }

        capture_output_index_ = chosen->global_index;
        capture_width_ = static_cast<std::uint32_t>(chosen->desc.DesktopCoordinates.right -
                                                    chosen->desc.DesktopCoordinates.left);
        capture_height_ = static_cast<std::uint32_t>(chosen->desc.DesktopCoordinates.bottom -
                                                     chosen->desc.DesktopCoordinates.top);

        DXGI_ADAPTER_DESC1 adapter_desc{};
        chosen->adapter->GetDesc1(&adapter_desc);
        std::wcout << L"Desktop capture output=" << capture_output_index_
                   << L" adapter=" << adapter_desc.Description
                   << L" size=" << capture_width_ << L"x" << capture_height_
                   << L" monitor=" << chosen->desc.DeviceName
                   << std::endl;

        UINT create_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(chosen->adapter.Get(),
                                       D3D_DRIVER_TYPE_UNKNOWN,
                                       nullptr,
                                       create_flags,
                                       &feature_level,
                                       1,
                                       D3D11_SDK_VERSION,
                                       &device_,
                                       nullptr,
                                       &context_);
        if (FAILED(hr) || !device_ || !context_) {
            std::cerr << "D3D11CreateDevice for capture failed.\n";
            return false;
        }

        ComPtr<IDXGIOutput1> output1;
        hr = chosen->output.As(&output1);
        if (FAILED(hr) || !output1) {
            std::cerr << "IDXGIOutput1 unavailable.\n";
            return false;
        }

        hr = output1->DuplicateOutput(device_.Get(), &duplication_);
        if (FAILED(hr) || !duplication_) {
            std::cerr << "DuplicateOutput failed. hr=0x" << std::hex << hr << std::dec << "\n";
            return false;
        }

        has_frame_ = false;
        if (out_width != nullptr) *out_width = capture_width_;
        if (out_height != nullptr) *out_height = capture_height_;
        return true;
    }

    ID3D11Device* device() const noexcept { return device_.Get(); }
    ID3D11DeviceContext* context() const noexcept { return context_.Get(); }

    bool UpdateFrameCache(ID3D11Texture2D* cache_texture) {
        if (duplication_ == nullptr || cache_texture == nullptr) {
            return false;
        }

        DXGI_OUTDUPL_FRAME_INFO frame_info{};
        ComPtr<IDXGIResource> resource;
        ScopedDuplicationFrame frame_guard{duplication_.Get(), false};
        const HRESULT hr = duplication_->AcquireNextFrame(0, &frame_info, &resource);
        if (hr == S_OK) {
            frame_guard.acquired = true;
            ComPtr<ID3D11Texture2D> desktop_texture;
            if (SUCCEEDED(resource.As(&desktop_texture)) && desktop_texture) {
                context_->CopyResource(cache_texture, desktop_texture.Get());
                has_frame_ = true;
            }
        } else if (hr == DXGI_ERROR_ACCESS_LOST) {
            std::cerr << "Desktop duplication access lost.\n";
            return false;
        } else if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
            std::cerr << "AcquireNextFrame failed. hr=0x" << std::hex << hr << std::dec << "\n";
            return false;
        }

        return has_frame_;
    }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    std::uint32_t capture_width_ = 0;
    std::uint32_t capture_height_ = 0;
    int capture_output_index_ = 0;
    bool has_frame_ = false;
};

class GpuFrameScaler final {
public:
    bool Initialize(ID3D11Device* device,
                    std::uint32_t src_width,
                    std::uint32_t src_height,
                    std::uint32_t dst_width,
                    std::uint32_t dst_height) {
        if (device == nullptr || src_width == 0 || src_height == 0 || dst_width == 0 || dst_height == 0) {
            return false;
        }

        src_width_ = src_width;
        src_height_ = src_height;
        dst_width_ = dst_width;
        dst_height_ = dst_height;

        D3D11_TEXTURE2D_DESC capture_desc{};
        capture_desc.Width = src_width_;
        capture_desc.Height = src_height_;
        capture_desc.MipLevels = 1;
        capture_desc.ArraySize = 1;
        capture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        capture_desc.SampleDesc.Count = 1;
        capture_desc.Usage = D3D11_USAGE_DEFAULT;
        capture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&capture_desc, nullptr, &capture_texture_)) || !capture_texture_) {
            std::cerr << "CreateTexture2D(capture cache) failed.\n";
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = capture_desc.Format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(capture_texture_.Get(), &srv_desc, &capture_srv_)) || !capture_srv_) {
            std::cerr << "CreateShaderResourceView failed.\n";
            return false;
        }

        D3D11_TEXTURE2D_DESC scaled_desc{};
        scaled_desc.Width = dst_width_;
        scaled_desc.Height = dst_height_;
        scaled_desc.MipLevels = 1;
        scaled_desc.ArraySize = 1;
        scaled_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scaled_desc.SampleDesc.Count = 1;
        scaled_desc.Usage = D3D11_USAGE_DEFAULT;
        scaled_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&scaled_desc, nullptr, &scaled_texture_)) || !scaled_texture_) {
            std::cerr << "CreateTexture2D(scaled target) failed.\n";
            return false;
        }

        if (FAILED(device->CreateRenderTargetView(scaled_texture_.Get(), nullptr, &scaled_rtv_)) || !scaled_rtv_) {
            std::cerr << "CreateRenderTargetView failed.\n";
            return false;
        }

        const char* kVertexShader = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vertexId : SV_VertexID) {
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0)
    };

    VSOut output;
    output.pos = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
)";

        const char* kPixelShader = R"(
Texture2D inputTexture : register(t0);
SamplerState linearSampler : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return inputTexture.Sample(linearSampler, uv);
}
)";

        ComPtr<ID3DBlob> vs_blob;
        ComPtr<ID3DBlob> ps_blob;
        if (FAILED(CompileShader(kVertexShader, "main", "vs_5_0", &vs_blob)) || !vs_blob) {
            return false;
        }
        if (FAILED(CompileShader(kPixelShader, "main", "ps_5_0", &ps_blob)) || !ps_blob) {
            return false;
        }

        if (FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                              vs_blob->GetBufferSize(),
                                              nullptr,
                                              &vertex_shader_)) ||
            !vertex_shader_) {
            std::cerr << "CreateVertexShader failed.\n";
            return false;
        }

        if (FAILED(device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                             ps_blob->GetBufferSize(),
                                             nullptr,
                                             &pixel_shader_)) ||
            !pixel_shader_) {
            std::cerr << "CreatePixelShader failed.\n";
            return false;
        }

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sampler_desc, &sampler_state_)) || !sampler_state_) {
            std::cerr << "CreateSamplerState failed.\n";
            return false;
        }

        return true;
    }

    ID3D11Texture2D* capture_texture() const noexcept { return capture_texture_.Get(); }
    ID3D11Texture2D* scaled_texture() const noexcept { return scaled_texture_.Get(); }

    bool RenderScaled(ID3D11DeviceContext* context) {
        if (context == nullptr || !capture_srv_ || !scaled_rtv_ || !vertex_shader_ || !pixel_shader_ || !sampler_state_) {
            return false;
        }

        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->OMSetRenderTargets(1, scaled_rtv_.GetAddressOf(), nullptr);
        context->ClearRenderTargetView(scaled_rtv_.Get(), clear_color);

        float viewport_width = static_cast<float>(dst_width_);
        float viewport_height = static_cast<float>(dst_height_);
        const float src_aspect = static_cast<float>(src_width_) / static_cast<float>(src_height_);
        const float dst_aspect = static_cast<float>(dst_width_) / static_cast<float>(dst_height_);

        if (src_aspect > dst_aspect) {
            viewport_height = static_cast<float>(dst_width_) / src_aspect;
        } else {
            viewport_width = static_cast<float>(dst_height_) * src_aspect;
        }

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.5f * (static_cast<float>(dst_width_) - viewport_width);
        viewport.TopLeftY = 0.5f * (static_cast<float>(dst_height_) - viewport_height);
        viewport.Width = viewport_width;
        viewport.Height = viewport_height;
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        ID3D11ShaderResourceView* srvs[] = {capture_srv_.Get()};
        ID3D11SamplerState* samplers[] = {sampler_state_.Get()};

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        context->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, srvs);
        context->PSSetSamplers(0, 1, samplers);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* null_srvs[] = {nullptr};
        ID3D11RenderTargetView* null_rtvs[] = {nullptr};
        context->PSSetShaderResources(0, 1, null_srvs);
        context->OMSetRenderTargets(1, null_rtvs, nullptr);
        return true;
    }

private:
    ComPtr<ID3D11Texture2D> capture_texture_;
    ComPtr<ID3D11ShaderResourceView> capture_srv_;
    ComPtr<ID3D11Texture2D> scaled_texture_;
    ComPtr<ID3D11RenderTargetView> scaled_rtv_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11SamplerState> sampler_state_;
    std::uint32_t src_width_ = 0;
    std::uint32_t src_height_ = 0;
    std::uint32_t dst_width_ = 0;
    std::uint32_t dst_height_ = 0;
};

class DesktopCaptureSource final : public vt::windows::IVideoContentSource {
public:
    DesktopCaptureSource(int output_index, std::uint16_t control_port)
        : output_index_(output_index), control_port_(control_port) {}

    bool Initialize(std::uint16_t* inout_width, std::uint16_t* inout_height) override {
        if (inout_width == nullptr || inout_height == nullptr) {
            return false;
        }

        if (!vt::windows::OpenNonBlockingUdpSocket(control_port_, "control", &control_socket_)) {
            return false;
        }

        if (!duplicator_.Initialize(output_index_, &desktop_width_, &desktop_height_)) {
            return false;
        }

        if (*inout_width == 0) {
            *inout_width = static_cast<std::uint16_t>(desktop_width_);
        }
        if (*inout_height == 0) {
            *inout_height = static_cast<std::uint16_t>(desktop_height_);
        }

        encode_width_ = *inout_width;
        encode_height_ = *inout_height;
        if (!scaler_.Initialize(
                duplicator_.device(), desktop_width_, desktop_height_, encode_width_, encode_height_)) {
            std::cerr << "GpuFrameScaler initialization failed.\n";
            return false;
        }

        return true;
    }

    bool UpdateAndRender(const vt::windows::FrameContext&,
                         vt::windows::ControlRequestState* control_state,
                         ID3D11Texture2D** out_texture) override {
        if (out_texture == nullptr) {
            return false;
        }
        *out_texture = nullptr;

        vt::windows::PollControlPackets(control_socket_, control_state);
        if (!duplicator_.UpdateFrameCache(scaler_.capture_texture())) {
            return false;
        }
        if (!scaler_.RenderScaled(duplicator_.context())) {
            std::cerr << "GpuFrameScaler render failed.\n";
            return false;
        }

        *out_texture = scaler_.scaled_texture();
        return true;
    }

    ID3D11Device* device() const noexcept override { return duplicator_.device(); }
    ID3D11DeviceContext* context() const noexcept override { return duplicator_.context(); }
    const char* SenderName() const noexcept override { return "desktop_capture_nvenc_sender"; }
    const char* FrameLogLabel() const noexcept override { return "desktop"; }
    std::uint32_t FrameLogInterval() const noexcept override { return 30; }

    std::string StartupDetails() const override {
        return "desktop=" + std::to_string(desktop_width_) + "x" + std::to_string(desktop_height_) +
               " encode=" + std::to_string(encode_width_) + "x" + std::to_string(encode_height_) +
               " output_index=" + std::to_string(output_index_) +
               " control_port=" + std::to_string(control_port_) +
               " path=gpu-scale";
    }

    void Shutdown() noexcept override {
        if (control_socket_ != INVALID_SOCKET) {
            closesocket(control_socket_);
            control_socket_ = INVALID_SOCKET;
        }
    }

private:
    int output_index_ = 0;
    std::uint16_t control_port_ = 25672;
    SOCKET control_socket_ = INVALID_SOCKET;
    std::uint32_t desktop_width_ = 0;
    std::uint32_t desktop_height_ = 0;
    std::uint16_t encode_width_ = 0;
    std::uint16_t encode_height_ = 0;
    DesktopDuplicator duplicator_;
    GpuFrameScaler scaler_;
};

}  // namespace

int main(int argc, char** argv) {
    SenderConfig config{};
    if (argc >= 2) config.host = argv[1];
    if (argc >= 3) {
        try { config.port = static_cast<std::uint16_t>(std::stoul(argv[2])); } catch (...) {}
    }
    if (argc >= 4) {
        try { config.fps = std::max(1, std::stoi(argv[3])); } catch (...) {}
    }
    if (argc >= 5) {
        try { config.bitrate = static_cast<std::uint32_t>(std::stoul(argv[4])); } catch (...) {}
    }
    if (argc >= 6) {
        try { config.output_index = std::max(0, std::stoi(argv[5])); } catch (...) {}
    }
    if (argc >= 7) {
        try { config.width = static_cast<std::uint16_t>(std::stoul(argv[6])); } catch (...) {}
    }
    if (argc >= 8) {
        try { config.height = static_cast<std::uint16_t>(std::stoul(argv[7])); } catch (...) {}
    }
    if (argc >= 9) {
        try { config.control_port = static_cast<std::uint16_t>(std::stoul(argv[8])); } catch (...) {}
    }

    vt::windows::SenderRuntimeConfig runtime{};
    runtime.host = config.host;
    runtime.video_port = config.port;
    runtime.width = config.width;
    runtime.height = config.height;
    runtime.fps = config.fps;
    runtime.bitrate = config.bitrate;

    DesktopCaptureSource source(config.output_index, config.control_port);
    return vt::windows::RunNvencVideoSender(runtime, &source);
}
