#pragma once

#include "egl_context.h"
#include "amedia_h264_decoder.h"
#include "udp_encoded_video_receiver.h"
#include "udp_pose_sender.h"
#include "udp_video_receiver.h"

#include <android/surface_texture.h>
#include <android_native_app_glue.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vt::android {

enum class DecodedVideoDisplayMode : std::uint16_t {
    QuadMono = 0,
    ProjectionMono = 1,
    ProjectionStereo = 2,
};

inline constexpr const char* DecodedVideoDisplayModeName(DecodedVideoDisplayMode mode) noexcept {
    switch (mode) {
        case DecodedVideoDisplayMode::ProjectionMono:
            return "projection_mono";
        case DecodedVideoDisplayMode::ProjectionStereo:
            return "projection_stereo";
        case DecodedVideoDisplayMode::QuadMono:
        default:
            return "quad_mono";
    }
}

struct RuntimeConfig final {
    std::string target_host = "192.168.1.100";
    std::uint16_t pose_target_port = 25672;
    std::uint16_t control_target_port = 25672;
    std::uint16_t video_port = 25673;
    std::uint16_t encoded_video_port = 25674;
    DecodedVideoDisplayMode display_mode = DecodedVideoDisplayMode::QuadMono;
};

class XrPoseRuntime final {
public:
    bool Initialize(android_app* app, const RuntimeConfig& config, std::string* error);
    void Shutdown();

    void PollEvents(bool* exit_render_loop, bool* request_restart);
    bool IsSessionRunning() const noexcept { return session_running_; }
    void TickBackgroundWork();
    void RunFrame();

private:
    static constexpr std::uint32_t kMaxDecodedVideoGlViews = 2;

    struct DecodedVideoGlView final {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        std::vector<GLuint> framebuffers;
        jobject surface_texture_obj = nullptr;
        ASurfaceTexture* surface_texture = nullptr;
        GLuint external_texture = 0;
        AMediaH264Decoder decoder{};
        bool logged_waiting_for_first_frame = false;
        bool logged_first_frame_ready = false;
    };

    bool InitializeLoader(android_app* app, std::string* error);
    bool CreateInstance(android_app* app, std::string* error);
    bool InitializeSystem(std::string* error);
    bool InitializeGraphics(std::string* error);
    bool CreateSession(std::string* error);
    bool InitializePrimaryStereoViews(std::string* error);
    bool CreateReferenceSpaces(std::string* error);
    bool CreateQuadSwapchain(std::string* error);
    void DestroyQuadSwapchain();
    bool CreateAndroidSurfaceSwapchain(std::string* error);
    void DestroyAndroidSurfaceSwapchain();
    bool CreateDecodedVideoGlOutput(std::string* error);
    bool CreateDecodedVideoGlOutputView(std::uint32_t view_index, std::string* error);
    void DestroyDecodedVideoGlOutput();
    void DestroyDecodedVideoGlOutputView(std::uint32_t view_index);
    bool RenderDecodedVideoToGlSwapchain(std::uint32_t view_index, bool vertical_flip);
    void PumpEncodedVideoDecoder();
    std::uint32_t TargetDecodedVideoGlViewCount() const noexcept;
    bool HasDecodedVideoForGlView(std::uint32_t view_index) const noexcept;
    std::uint16_t CurrentDecodedGlStreamFlags(std::uint32_t view_index) const noexcept;

    void HandleSessionStateChanged(const XrEventDataSessionStateChanged& changed_event,
                                   bool* exit_render_loop,
                                   bool* request_restart);
    bool SupportsExtension(const char* extension_name) const;
    bool SendHeadPose(XrTime predicted_display_time);
    bool SendControlMessage(vt::proto::ControlMessageType message_type,
                            std::uint32_t related_frame_id,
                            std::uint16_t flags = vt::proto::ControlMessageFlagNone,
                            std::uint32_t value0 = 0,
                            std::uint32_t value1 = 0);
    vt::proto::PosePayload BuildPosePayload(XrTime predicted_display_time) const;
    bool TryBuildProjectionLayerFromSwapchain(XrTime predicted_display_time,
                                              XrSwapchain swapchain,
                                              std::uint32_t image_width,
                                              std::uint32_t image_height,
                                              XrCompositionLayerProjection* layer,
                                              std::vector<XrCompositionLayerProjectionView>* projection_views);
    bool TryBuildProjectionLayerFromSwapchains(XrTime predicted_display_time,
                                               const XrSwapchain* swapchains,
                                               std::uint32_t swapchain_count,
                                               std::uint32_t image_width,
                                               std::uint32_t image_height,
                                               XrCompositionLayerProjection* layer,
                                               std::vector<XrCompositionLayerProjectionView>* projection_views);
    bool TryBuildDecodedProjectionLayer(XrTime predicted_display_time,
                                         XrCompositionLayerProjection* layer,
                                         std::vector<XrCompositionLayerProjectionView>* projection_views);
    bool TryBuildDecodedGlProjectionLayer(XrTime predicted_display_time,
                                           XrCompositionLayerProjection* layer,
                                           std::vector<XrCompositionLayerProjectionView>* projection_views);
    bool TryBuildDecodedGlStereoProjectionLayer(XrTime predicted_display_time,
                                                XrCompositionLayerProjection* layer,
                                                std::vector<XrCompositionLayerProjectionView>* projection_views);
    bool RenderQuadLayer(XrTime predicted_display_time, XrCompositionLayerQuad* layer);
    bool UsesDecodedVideoGlProjection() const noexcept;

    android_app* app_ = nullptr;
    RuntimeConfig config_{};
    UdpPoseSender pose_sender_{};
    UdpPoseSender control_sender_{};
    UdpVideoReceiver video_receiver_{};
    UdpEncodedVideoReceiver encoded_video_receiver_{};
    AMediaH264Decoder h264_decoder_{};
    EglContext egl_{};

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_id_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace app_space_ = XR_NULL_HANDLE;
    XrSpace head_space_ = XR_NULL_HANDLE;
    XrSwapchain quad_swapchain_ = XR_NULL_HANDLE;
    XrSwapchain android_surface_swapchain_ = XR_NULL_HANDLE;
    XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
    XrEnvironmentBlendMode blend_mode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_gles_requirements_ = nullptr;
    PFN_xrCreateSwapchainAndroidSurfaceKHR create_swapchain_android_surface_khr_ = nullptr;
    PFN_xrUpdateSwapchainFB update_swapchain_fb_ = nullptr;

    std::vector<std::string> available_extensions_;
    std::vector<XrViewConfigurationView> primary_stereo_view_configs_;
    std::vector<XrView> primary_stereo_views_;
    std::vector<XrSwapchainImageOpenGLESKHR> quad_images_;
    std::vector<GLuint> quad_framebuffers_;
    jobject android_surface_obj_ = nullptr;
    std::array<DecodedVideoGlView, kMaxDecodedVideoGlViews> decoded_video_gl_views_{};
    GLuint decoded_blit_program_ = 0;
    GLuint decoded_blit_vbo_ = 0;
    GLint decoded_blit_position_attrib_ = -1;
    GLint decoded_blit_texcoord_attrib_ = -1;
    GLint decoded_blit_texture_uniform_ = -1;
    GLint decoded_blit_transform_uniform_ = -1;
    GLint decoded_blit_vertical_flip_uniform_ = -1;
    bool session_running_ = false;
    std::uint32_t pose_sequence_ = 0;
    std::uint32_t control_sequence_ = 0;
    std::uint64_t frames_sent_ = 0;
    int64_t quad_swapchain_format_ = 0;
    int64_t decoded_video_gl_swapchain_format_ = 0;
    std::uint32_t decoded_video_gl_view_count_ = 0;
    std::uint32_t quad_width_ = 160;
    std::uint32_t quad_height_ = 90;
    std::uint32_t encoded_width_ = 640;
    std::uint32_t encoded_height_ = 360;
    bool logged_waiting_for_decoded_frame_ = false;
    bool logged_surface_layer_active_ = false;
    bool logged_projection_layer_active_ = false;
    bool logged_projection_stereo_waiting_ = false;
    bool logged_projection_stereo_active_ = false;
    bool logged_projection_build_failure_ = false;
    bool composition_layer_image_layout_enabled_ = false;
};

}  // namespace vt::android
