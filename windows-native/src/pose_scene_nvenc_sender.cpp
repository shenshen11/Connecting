#include "packet_defs.h"
#include "control_protocol.h"
#include "pose_protocol.h"
#include "video_protocol.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
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
constexpr float kPi = 3.1415926535f;

struct SenderConfig final {
    std::string host = "127.0.0.1";
    std::uint16_t video_port = 25674;
    std::uint16_t pose_port = 25672;
    std::uint16_t width = 1280;
    std::uint16_t height = 720;
    int fps = 30;
    std::uint32_t bitrate = 4'000'000;
};

struct PoseState final {
    vt::proto::PosePayload pose{};
    std::uint32_t sequence = 0;
    bool has_pose = false;
    std::uint64_t packets_received = 0;
};

struct SceneConstants final {
    float camera_pos_time[4];
    float rot_row0[4];
    float rot_row1[4];
    float rot_row2[4];
    float params[4];
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
        std::cerr << "Shader compilation failed: "
                  << static_cast<const char*>(error_blob->GetBufferPointer()) << "\n";
    }
    return hr;
}

bool OpenPoseSocket(std::uint16_t port, SOCKET* out_socket) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "pose socket() failed.\n";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "pose bind() failed on port " << port << ".\n";
        closesocket(sock);
        return false;
    }

    u_long non_blocking = 1;
    if (ioctlsocket(sock, FIONBIO, &non_blocking) == SOCKET_ERROR) {
        std::cerr << "ioctlsocket(FIONBIO) failed.\n";
        closesocket(sock);
        return false;
    }

    *out_socket = sock;
    return true;
}

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

void QuaternionToMatrixRows(const vt::proto::Quatf& q, float row0[4], float row1[4], float row2[4]) {
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float xw = q.x * q.w;
    const float yw = q.y * q.w;
    const float zw = q.z * q.w;

    row0[0] = 1.0f - 2.0f * (yy + zz);
    row0[1] = 2.0f * (xy - zw);
    row0[2] = 2.0f * (xz + yw);
    row0[3] = 0.0f;

    row1[0] = 2.0f * (xy + zw);
    row1[1] = 1.0f - 2.0f * (xx + zz);
    row1[2] = 2.0f * (yz - xw);
    row1[3] = 0.0f;

    row2[0] = 2.0f * (xz - yw);
    row2[1] = 2.0f * (yz + xw);
    row2[2] = 1.0f - 2.0f * (xx + yy);
    row2[3] = 0.0f;
}

void PollPosePackets(SOCKET sock, PoseState* state, vt::windows::ControlRequestState* control_state) {
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
                std::cerr << "recvfrom(pose) failed: " << error << "\n";
            }
            break;
        }

        if (bytes < static_cast<int>(sizeof(vt::proto::PacketHeader))) {
            continue;
        }

        vt::proto::PacketHeader header{};
        std::memcpy(&header, buffer, sizeof(header));
        if (!vt::proto::IsValidHeader(header)) {
            continue;
        }

        if (header.type == static_cast<std::uint16_t>(vt::proto::PacketType::Control)) {
            if (header.payload_size != sizeof(vt::proto::ControlPayload) ||
                bytes < static_cast<int>(sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::ControlPayload))) {
                continue;
            }

            vt::proto::ControlPayload payload{};
            std::memcpy(&payload, buffer + sizeof(header), sizeof(payload));
            if (control_state != nullptr) {
                vt::windows::HandleControlPayload(payload, control_state);
            }
            continue;
        }

        if (header.type != static_cast<std::uint16_t>(vt::proto::PacketType::Pose) ||
            header.payload_size != sizeof(vt::proto::PosePayload) ||
            bytes < static_cast<int>(sizeof(vt::proto::PacketHeader) + sizeof(vt::proto::PosePayload))) {
            continue;
        }

        vt::proto::PosePayload pose{};
        std::memcpy(&pose, buffer + sizeof(header), sizeof(pose));
        NormalizeQuaternion(&pose.orientation);

        state->pose = pose;
        state->sequence = header.sequence;
        state->has_pose = true;
        state->packets_received += 1;

        if (state->packets_received <= 5 || (state->packets_received % 120) == 0) {
            std::cout << "pose update seq=" << header.sequence
                      << " pos=(" << pose.position_m.x << ", " << pose.position_m.y << ", " << pose.position_m.z
                      << ") quat=(" << pose.orientation.x << ", " << pose.orientation.y << ", "
                      << pose.orientation.z << ", " << pose.orientation.w << ")"
                      << std::endl;
        }
    }
}

class ProceduralSceneRenderer final {
public:
    bool Initialize(ID3D11Device* device, std::uint32_t width, std::uint32_t height) {
        width_ = width;
        height_ = height;

        D3D11_TEXTURE2D_DESC texture_desc{};
        texture_desc.Width = width_;
        texture_desc.Height = height_;
        texture_desc.MipLevels = 1;
        texture_desc.ArraySize = 1;
        texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture_desc.SampleDesc.Count = 1;
        texture_desc.Usage = D3D11_USAGE_DEFAULT;
        texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&texture_desc, nullptr, &scene_texture_)) || !scene_texture_) {
            std::cerr << "CreateTexture2D(scene texture) failed.\n";
            return false;
        }

        if (FAILED(device->CreateRenderTargetView(scene_texture_.Get(), nullptr, &scene_rtv_)) || !scene_rtv_) {
            std::cerr << "CreateRenderTargetView(scene texture) failed.\n";
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
cbuffer SceneCB : register(b0) {
    float4 cameraPosTime;
    float4 rotRow0;
    float4 rotRow1;
    float4 rotRow2;
    float4 params; // aspect, tanHalfFov, time, poseActive
}

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float sphereIntersect(float3 ro, float3 rd, float3 center, float radius) {
    float3 oc = ro - center;
    float b = dot(oc, rd);
    float c = dot(oc, oc) - radius * radius;
    float h = b * b - c;
    if (h < 0.0) return -1.0;
    h = sqrt(h);
    float t = -b - h;
    return (t > 0.0) ? t : (-b + h);
}

float3 skyColor(float3 rd) {
    float t = saturate(rd.y * 0.5 + 0.5);
    float3 bottom = float3(0.04, 0.05, 0.08);
    float3 top = float3(0.35, 0.55, 0.90);
    return lerp(bottom, top, t);
}

float3 shadeFloor(float3 p) {
    float2 gridUv = p.xz;
    float2 fw = max(fwidth(gridUv), float2(0.001, 0.001));
    float2 grid = abs(frac(gridUv) - 0.5) / fw;
    float gridLine = 1.0 - saturate(min(grid.x, grid.y));

    float axisX = 1.0 - saturate(abs(p.x) / 0.03);
    float axisZ = 1.0 - saturate(abs(p.z) / 0.03);

    float checker = frac(floor(p.x) + floor(p.z)) * 0.03;
    float3 color = float3(0.08, 0.08, 0.09) + checker;
    color += gridLine * 0.18;
    color = lerp(color, float3(0.95, 0.25, 0.20), axisX);
    color = lerp(color, float3(0.15, 0.65, 1.00), axisZ);

    float falloff = 1.0 / (1.0 + 0.03 * dot(p.xz, p.xz));
    return color * falloff;
}

float3 shadeSphere(float3 p, float3 center, float3 albedo) {
    float3 normal = normalize(p - center);
    float3 lightDir = normalize(float3(0.6, 1.0, -0.35));
    float diffuse = saturate(dot(normal, lightDir));
    float rim = pow(1.0 - saturate(normal.z * 0.5 + 0.5), 2.0);
    return albedo * (0.18 + 0.82 * diffuse) + rim * 0.08;
}

float4 main(PSInput input) : SV_TARGET {
    float2 uv = input.uv * 2.0 - 1.0;
    float2 plane = float2(uv.x * params.x * params.y, -uv.y * params.y);
    float3 dirCamera = normalize(float3(plane, -1.0));
    float3 dirWorld = float3(dot(rotRow0.xyz, dirCamera),
                             dot(rotRow1.xyz, dirCamera),
                             dot(rotRow2.xyz, dirCamera));
    float3 ro = cameraPosTime.xyz;
    float time = cameraPosTime.w;

    float3 color = skyColor(dirWorld);
    float closestT = 1e9;

    float3 sphere0 = float3(0.0, 1.15, -3.0);
    float3 sphere1 = float3(-1.4, 0.85, -4.3);
    float3 sphere2 = float3(1.35 + sin(time * 0.8) * 0.25, 0.9 + sin(time * 1.1) * 0.08, -4.8);

    float t0 = sphereIntersect(ro, dirWorld, sphere0, 0.55);
    if (t0 > 0.0 && t0 < closestT) {
        closestT = t0;
        color = shadeSphere(ro + dirWorld * t0, sphere0, float3(0.98, 0.40, 0.24));
    }

    float t1 = sphereIntersect(ro, dirWorld, sphere1, 0.42);
    if (t1 > 0.0 && t1 < closestT) {
        closestT = t1;
        color = shadeSphere(ro + dirWorld * t1, sphere1, float3(0.20, 0.80, 1.00));
    }

    float t2 = sphereIntersect(ro, dirWorld, sphere2, 0.48);
    if (t2 > 0.0 && t2 < closestT) {
        closestT = t2;
        color = shadeSphere(ro + dirWorld * t2, sphere2, float3(0.75, 0.95, 0.25));
    }

    if (dirWorld.y < -0.001) {
        float floorT = (0.0 - ro.y) / dirWorld.y;
        if (floorT > 0.0 && floorT < closestT) {
            color = shadeFloor(ro + dirWorld * floorT);
        }
    }

    float vignette = saturate(1.15 - dot(uv * 0.65, uv * 0.65));
    color *= vignette;
    return float4(color, 1.0);
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

        D3D11_BUFFER_DESC cb_desc{};
        cb_desc.ByteWidth = sizeof(SceneConstants);
        cb_desc.Usage = D3D11_USAGE_DYNAMIC;
        cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cb_desc, nullptr, &constant_buffer_)) || !constant_buffer_) {
            std::cerr << "CreateBuffer(constant buffer) failed.\n";
            return false;
        }

        return true;
    }

    ID3D11Texture2D* texture() const noexcept { return scene_texture_.Get(); }

    bool Render(ID3D11DeviceContext* context,
                const PoseState& pose_state,
                float time_seconds,
                std::uint32_t frame_index) {
        if (context == nullptr || !scene_rtv_ || !vertex_shader_ || !pixel_shader_ || !constant_buffer_) {
            return false;
        }

        SceneConstants constants{};
        vt::proto::PosePayload pose = pose_state.pose;
        if (!pose_state.has_pose) {
            pose.position_m = {0.0f, 1.60f, 0.0f};
            pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        }
        NormalizeQuaternion(&pose.orientation);

        constants.camera_pos_time[0] = pose.position_m.x;
        constants.camera_pos_time[1] = std::max(0.2f, pose.position_m.y);
        constants.camera_pos_time[2] = pose.position_m.z;
        constants.camera_pos_time[3] = time_seconds;
        QuaternionToMatrixRows(pose.orientation, constants.rot_row0, constants.rot_row1, constants.rot_row2);
        constants.params[0] = static_cast<float>(width_) / static_cast<float>(height_);
        constants.params[1] = std::tan((70.0f * kPi / 180.0f) * 0.5f);
        constants.params[2] = static_cast<float>(frame_index);
        constants.params[3] = pose_state.has_pose ? 1.0f : 0.0f;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(constant_buffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return false;
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(constant_buffer_.Get(), 0);

        const float clear_color[4] = {0.03f, 0.04f, 0.06f, 1.0f};
        context->OMSetRenderTargets(1, scene_rtv_.GetAddressOf(), nullptr);
        context->ClearRenderTargetView(scene_rtv_.Get(), clear_color);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width_);
        viewport.Height = static_cast<float>(height_);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        ID3D11Buffer* cbs[] = {constant_buffer_.Get()};
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        context->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, cbs);
        context->Draw(3, 0);

        ID3D11RenderTargetView* null_rtvs[] = {nullptr};
        context->OMSetRenderTargets(1, null_rtvs, nullptr);
        return true;
    }

private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    ComPtr<ID3D11Texture2D> scene_texture_;
    ComPtr<ID3D11RenderTargetView> scene_rtv_;
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11Buffer> constant_buffer_;
};

class PoseDrivenSceneSource final : public vt::windows::IVideoContentSource {
public:
    explicit PoseDrivenSceneSource(std::uint16_t pose_port) : pose_port_(pose_port) {}

    bool Initialize(std::uint16_t* inout_width, std::uint16_t* inout_height) override {
        if (inout_width == nullptr || inout_height == nullptr) {
            return false;
        }

        if (*inout_width == 0) {
            *inout_width = 1280;
        }
        if (*inout_height == 0) {
            *inout_height = 720;
        }

        width_ = *inout_width;
        height_ = *inout_height;

        if (!vt::windows::OpenNonBlockingUdpSocket(pose_port_, "pose", &pose_socket_)) {
            return false;
        }

        const HRESULT hr = D3D11CreateDevice(nullptr,
                                             D3D_DRIVER_TYPE_HARDWARE,
                                             nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                             nullptr,
                                             0,
                                             D3D11_SDK_VERSION,
                                             device_.GetAddressOf(),
                                             nullptr,
                                             context_.GetAddressOf());
        if (FAILED(hr) || !device_ || !context_) {
            std::cerr << "D3D11CreateDevice failed.\n";
            return false;
        }

        if (!renderer_.Initialize(device_.Get(), width_, height_)) {
            std::cerr << "renderer.Initialize failed.\n";
            return false;
        }

        pose_state_.pose.position_m = {0.0f, 1.60f, 0.0f};
        pose_state_.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        return true;
    }

    bool UpdateAndRender(const vt::windows::FrameContext& frame_context,
                         vt::windows::ControlRequestState* control_state,
                         ID3D11Texture2D** out_texture) override {
        if (out_texture == nullptr) {
            return false;
        }
        *out_texture = nullptr;

        PollPosePackets(pose_socket_, &pose_state_, control_state);
        if (!renderer_.Render(
                context_.Get(), pose_state_, static_cast<float>(frame_context.time_seconds), frame_context.frame_index)) {
            std::cerr << "renderer.Render failed.\n";
            return false;
        }

        if (!pose_state_.has_pose && !warned_no_pose_ && frame_context.time_seconds > 2.0) {
            std::cout << "warning: no pose packets received on UDP port " << pose_port_
                      << "; scene is using default camera. Check headset app foreground state and Windows firewall."
                      << std::endl;
            warned_no_pose_ = true;
        }
        if (pose_state_.has_pose && !announced_pose_active_) {
            std::cout << "pose stream active on UDP port " << pose_port_ << std::endl;
            announced_pose_active_ = true;
        }

        *out_texture = renderer_.texture();
        return true;
    }

    ID3D11Device* device() const noexcept override { return device_.Get(); }
    ID3D11DeviceContext* context() const noexcept override { return context_.Get(); }
    const char* SenderName() const noexcept override { return "pose_scene_nvenc_sender"; }
    const char* FrameLogLabel() const noexcept override { return "scene"; }
    std::uint32_t FrameLogInterval() const noexcept override { return 60; }
    std::string StartupDetails() const override { return "pose_port=" + std::to_string(pose_port_); }

    std::string FrameLogSuffix() const override {
        return "pose_packets=" + std::to_string(pose_state_.packets_received) +
               (pose_state_.has_pose ? " pose=active" : " pose=default");
    }

    void Shutdown() noexcept override {
        if (pose_socket_ != INVALID_SOCKET) {
            closesocket(pose_socket_);
            pose_socket_ = INVALID_SOCKET;
        }
        context_.Reset();
        device_.Reset();
    }

private:
    std::uint16_t pose_port_ = 25672;
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    SOCKET pose_socket_ = INVALID_SOCKET;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    PoseState pose_state_{};
    ProceduralSceneRenderer renderer_;
    bool warned_no_pose_ = false;
    bool announced_pose_active_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    SenderConfig config{};
    if (argc >= 2) config.host = argv[1];
    if (argc >= 3) {
        try { config.video_port = static_cast<std::uint16_t>(std::stoul(argv[2])); } catch (...) {}
    }
    if (argc >= 4) {
        try { config.width = static_cast<std::uint16_t>(std::stoul(argv[3])); } catch (...) {}
    }
    if (argc >= 5) {
        try { config.height = static_cast<std::uint16_t>(std::stoul(argv[4])); } catch (...) {}
    }
    if (argc >= 6) {
        try { config.fps = std::max(1, std::stoi(argv[5])); } catch (...) {}
    }
    if (argc >= 7) {
        try { config.bitrate = static_cast<std::uint32_t>(std::stoul(argv[6])); } catch (...) {}
    }
    if (argc >= 8) {
        try { config.pose_port = static_cast<std::uint16_t>(std::stoul(argv[7])); } catch (...) {}
    }

    vt::windows::SenderRuntimeConfig runtime{};
    runtime.host = config.host;
    runtime.video_port = config.video_port;
    runtime.width = config.width;
    runtime.height = config.height;
    runtime.fps = config.fps;
    runtime.bitrate = config.bitrate;

    PoseDrivenSceneSource source(config.pose_port);
    return vt::windows::RunNvencVideoSender(runtime, &source);
}
