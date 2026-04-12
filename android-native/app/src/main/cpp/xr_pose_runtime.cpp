#include "xr_pose_runtime.h"

#include "control_protocol.h"
#include "runtime_config_store.h"
#include "time_sync.h"

#include <android/log.h>
#include <android/native_window_jni.h>

#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

namespace vt::android {
namespace {

constexpr const char* kLogTag = "videotest-native";
constexpr XrReferenceSpaceType kAppSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
constexpr XrReferenceSpaceType kHeadSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;

std::string XrResultToString(XrInstance instance, XrResult result) {
    char buffer[XR_MAX_RESULT_STRING_SIZE]{};
    if (instance != XR_NULL_HANDLE) {
        xrResultToString(instance, result, buffer);
        return buffer;
    }

    std::ostringstream oss;
    oss << "XrResult(" << result << ")";
    return oss.str();
}

std::string MakeError(const std::string& step, XrInstance instance, XrResult result) {
    std::ostringstream oss;
    oss << step << " failed: " << XrResultToString(instance, result);
    return oss.str();
}

std::uint32_t ToTrackingFlags(XrSpaceLocationFlags flags) {
    std::uint32_t result = static_cast<std::uint32_t>(vt::proto::TrackingFlags::None);
    if ((flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
        result |= static_cast<std::uint32_t>(vt::proto::TrackingFlags::OrientationValid);
    }
    if ((flags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
        result |= static_cast<std::uint32_t>(vt::proto::TrackingFlags::PositionValid);
    }
    if ((flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0) {
        result |= static_cast<std::uint32_t>(vt::proto::TrackingFlags::OrientationTracked);
    }
    if ((flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0) {
        result |= static_cast<std::uint32_t>(vt::proto::TrackingFlags::PositionTracked);
    }
    return result;
}

}  // namespace

bool XrPoseRuntime::Initialize(android_app* app, const RuntimeConfig& config, std::string* error) {
    app_ = app;
    config_ = config;

    SenderConfig pose_sender_config{};
    pose_sender_config.host = config_.target_host;
    pose_sender_config.port = config_.pose_target_port;
    if (!pose_sender_.Open(pose_sender_config)) {
        if (error != nullptr) {
            *error = "Failed to open UDP pose sender.";
        }
        return false;
    }

    SenderConfig control_sender_config{};
    control_sender_config.host = config_.target_host;
    control_sender_config.port = config_.control_target_port;
    if (!control_sender_.Open(control_sender_config)) {
        pose_sender_.Close();
        if (error != nullptr) {
            *error = "Failed to open UDP control sender.";
        }
        return false;
    }

    if (!video_receiver_.Start(config_.video_port)) {
        control_sender_.Close();
        pose_sender_.Close();
        if (error != nullptr) {
            *error = "Failed to start UDP video receiver.";
        }
        return false;
    }

    if (!encoded_video_receiver_.Start(config_.encoded_video_port)) {
        video_receiver_.Stop();
        control_sender_.Close();
        pose_sender_.Close();
        if (error != nullptr) {
            *error = "Failed to start UDP encoded-video receiver.";
        }
        return false;
    }

    if (!InitializeLoader(app, error)) {
        return false;
    }
    if (!CreateInstance(app, error)) {
        return false;
    }
    if (!InitializeSystem(error)) {
        return false;
    }
    if (!InitializeGraphics(error)) {
        return false;
    }
    if (!CreateSession(error)) {
        return false;
    }
    if (!CreateReferenceSpaces(error)) {
        return false;
    }
    if (!CreateQuadSwapchain(error)) {
        return false;
    }
    if (!CreateAndroidSurfaceSwapchain(error)) {
        return false;
    }

    __android_log_print(
        ANDROID_LOG_INFO,
        kLogTag,
        "OpenXR runtime initialized. pose_target=%s:%u control_target=%s:%u",
        config_.target_host.c_str(),
        static_cast<unsigned>(config_.pose_target_port),
        config_.target_host.c_str(),
        static_cast<unsigned>(config_.control_target_port));

    return true;
}

void XrPoseRuntime::Shutdown() {
    DestroyAndroidSurfaceSwapchain();
    DestroyQuadSwapchain();

    if (head_space_ != XR_NULL_HANDLE) {
        xrDestroySpace(head_space_);
        head_space_ = XR_NULL_HANDLE;
    }
    if (app_space_ != XR_NULL_HANDLE) {
        xrDestroySpace(app_space_);
        app_space_ = XR_NULL_HANDLE;
    }
    if (session_ != XR_NULL_HANDLE) {
        xrDestroySession(session_);
        session_ = XR_NULL_HANDLE;
    }
    if (instance_ != XR_NULL_HANDLE) {
        xrDestroyInstance(instance_);
        instance_ = XR_NULL_HANDLE;
    }

    egl_.Shutdown();
    encoded_video_receiver_.Stop();
    video_receiver_.Stop();
    control_sender_.Close();
    pose_sender_.Close();

    session_running_ = false;
    session_state_ = XR_SESSION_STATE_UNKNOWN;
    system_id_ = XR_NULL_SYSTEM_ID;
}

bool XrPoseRuntime::InitializeLoader(android_app* app, std::string* error) {
    PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
    const XrResult get_proc_result = xrGetInstanceProcAddr(
        XR_NULL_HANDLE,
        "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&initialize_loader));
    if (XR_FAILED(get_proc_result) || initialize_loader == nullptr) {
        if (error != nullptr) {
            *error = "xrInitializeLoaderKHR is unavailable.";
        }
        return false;
    }

    XrLoaderInitInfoAndroidKHR loader_init_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader_init_info.applicationVM = app->activity->vm;
    loader_init_info.applicationContext = app->activity->clazz;

    const XrResult init_result = initialize_loader(
        reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_init_info));
    if (XR_FAILED(init_result)) {
        if (error != nullptr) {
            *error = MakeError("xrInitializeLoaderKHR", XR_NULL_HANDLE, init_result);
        }
        return false;
    }

    return true;
}

bool XrPoseRuntime::CreateInstance(android_app* app, std::string* error) {
    uint32_t extension_count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr);
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateInstanceExtensionProperties(count)", XR_NULL_HANDLE, result);
        }
        return false;
    }

    std::vector<XrExtensionProperties> extensions(extension_count, {XR_TYPE_EXTENSION_PROPERTIES});
    result = xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extensions.data());
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateInstanceExtensionProperties(list)", XR_NULL_HANDLE, result);
        }
        return false;
    }

    available_extensions_.clear();
    for (const auto& extension : extensions) {
        available_extensions_.emplace_back(extension.extensionName);
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "OpenXR extension: %s", extension.extensionName);
    }

    if (!SupportsExtension(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME) ||
        !SupportsExtension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME) ||
        !SupportsExtension(XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME) ||
        !SupportsExtension(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME) ||
        !SupportsExtension(XR_FB_SWAPCHAIN_UPDATE_STATE_ANDROID_SURFACE_EXTENSION_NAME)) {
        if (error != nullptr) {
            *error =
                "Required OpenXR extensions missing for pose/video runtime.";
        }
        return false;
    }

    std::vector<const char*> required_extensions = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME,
        XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME,
        XR_FB_SWAPCHAIN_UPDATE_STATE_ANDROID_SURFACE_EXTENSION_NAME};

    composition_layer_image_layout_enabled_ = SupportsExtension(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
    if (composition_layer_image_layout_enabled_) {
        required_extensions.push_back(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "Enabling OpenXR extension: %s",
                            XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
    }

    XrInstanceCreateInfoAndroidKHR android_create_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_create_info.applicationVM = app->activity->vm;
    android_create_info.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
    create_info.next = &android_create_info;
    create_info.enabledExtensionCount = static_cast<uint32_t>(required_extensions.size());
    create_info.enabledExtensionNames = required_extensions.data();
    create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    std::strncpy(create_info.applicationInfo.applicationName,
                 "videotest-pose-sender",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(create_info.applicationInfo.engineName,
                 "videotest-native",
                 XR_MAX_ENGINE_NAME_SIZE - 1);
    create_info.applicationInfo.applicationVersion = 1;
    create_info.applicationInfo.engineVersion = 1;

    result = xrCreateInstance(&create_info, &instance_);
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrCreateInstance", XR_NULL_HANDLE, result);
        }
        return false;
    }

    return true;
}

bool XrPoseRuntime::InitializeSystem(std::string* error) {
    XrSystemGetInfo system_get_info{XR_TYPE_SYSTEM_GET_INFO};
    system_get_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    const XrResult get_system_result = xrGetSystem(instance_, &system_get_info, &system_id_);
    if (XR_FAILED(get_system_result)) {
        if (error != nullptr) {
            *error = MakeError("xrGetSystem", instance_, get_system_result);
        }
        return false;
    }

    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    const XrResult properties_result = xrGetSystemProperties(instance_, system_id_, &system_properties);
    if (XR_SUCCEEDED(properties_result)) {
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "OpenXR system: vendorId=%u name=%s maxLayerCount=%u",
                            system_properties.vendorId,
                            system_properties.systemName,
                            system_properties.graphicsProperties.maxLayerCount);
    }

    uint32_t blend_mode_count = 0;
    xrEnumerateEnvironmentBlendModes(
        instance_, system_id_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blend_mode_count, nullptr);
    if (blend_mode_count > 0) {
        std::vector<XrEnvironmentBlendMode> blend_modes(blend_mode_count);
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(instance_,
                                                          system_id_,
                                                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                          blend_mode_count,
                                                          &blend_mode_count,
                                                          blend_modes.data()))) {
            blend_mode_ = blend_modes.front();
            for (const auto mode : blend_modes) {
                if (mode == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
                    blend_mode_ = mode;
                    break;
                }
            }
        }
    }

    return true;
}

bool XrPoseRuntime::InitializeGraphics(std::string* error) {
    if (!egl_.Initialize(error)) {
        return false;
    }

    const XrResult proc_result =
        xrGetInstanceProcAddr(instance_,
                              "xrGetOpenGLESGraphicsRequirementsKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&get_gles_requirements_));
    if (XR_FAILED(proc_result) || get_gles_requirements_ == nullptr) {
        if (error != nullptr) {
            *error = MakeError("xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)", instance_, proc_result);
        }
        return false;
    }

    XrGraphicsRequirementsOpenGLESKHR graphics_requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    const XrResult requirements_result = get_gles_requirements_(instance_, system_id_, &graphics_requirements);
    if (XR_FAILED(requirements_result)) {
        if (error != nullptr) {
            *error = MakeError("xrGetOpenGLESGraphicsRequirementsKHR", instance_, requirements_result);
        }
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "OpenGLES requirements: min=%llu max=%llu",
                        static_cast<unsigned long long>(graphics_requirements.minApiVersionSupported),
                        static_cast<unsigned long long>(graphics_requirements.maxApiVersionSupported));

    XrResult result = xrGetInstanceProcAddr(instance_,
                                            "xrCreateSwapchainAndroidSurfaceKHR",
                                            reinterpret_cast<PFN_xrVoidFunction*>(&create_swapchain_android_surface_khr_));
    if (XR_FAILED(result) || create_swapchain_android_surface_khr_ == nullptr) {
        if (error != nullptr) {
            *error = MakeError("xrGetInstanceProcAddr(xrCreateSwapchainAndroidSurfaceKHR)", instance_, result);
        }
        return false;
    }

    result = xrGetInstanceProcAddr(instance_,
                                   "xrUpdateSwapchainFB",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&update_swapchain_fb_));
    if (XR_FAILED(result) || update_swapchain_fb_ == nullptr) {
        if (error != nullptr) {
            *error = MakeError("xrGetInstanceProcAddr(xrUpdateSwapchainFB)", instance_, result);
        }
        return false;
    }

    return true;
}

bool XrPoseRuntime::CreateSession(std::string* error) {
    XrGraphicsBindingOpenGLESAndroidKHR graphics_binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    graphics_binding.display = egl_.display();
    graphics_binding.config = egl_.config();
    graphics_binding.context = egl_.context();

    XrSessionCreateInfo session_create_info{XR_TYPE_SESSION_CREATE_INFO};
    session_create_info.next = &graphics_binding;
    session_create_info.systemId = system_id_;

    const XrResult create_result = xrCreateSession(instance_, &session_create_info, &session_);
    if (XR_FAILED(create_result)) {
        if (error != nullptr) {
            *error = MakeError("xrCreateSession", instance_, create_result);
        }
        return false;
    }

    return true;
}

bool XrPoseRuntime::CreateReferenceSpaces(std::string* error) {
    XrReferenceSpaceCreateInfo app_space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    app_space_create_info.referenceSpaceType = kAppSpaceType;
    app_space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

    XrResult result = xrCreateReferenceSpace(session_, &app_space_create_info, &app_space_);
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrCreateReferenceSpace(app/local)", instance_, result);
        }
        return false;
    }

    XrReferenceSpaceCreateInfo head_space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    head_space_create_info.referenceSpaceType = kHeadSpaceType;
    head_space_create_info.poseInReferenceSpace.orientation.w = 1.0f;
    result = xrCreateReferenceSpace(session_, &head_space_create_info, &head_space_);
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrCreateReferenceSpace(head/view)", instance_, result);
        }
        return false;
    }

    return true;
}

bool XrPoseRuntime::CreateQuadSwapchain(std::string* error) {
    uint32_t format_count = 0;
    XrResult result = xrEnumerateSwapchainFormats(session_, 0, &format_count, nullptr);
    if (XR_FAILED(result) || format_count == 0) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateSwapchainFormats(count)", instance_, result);
        }
        return false;
    }

    std::vector<int64_t> formats(format_count);
    result = xrEnumerateSwapchainFormats(session_, format_count, &format_count, formats.data());
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateSwapchainFormats(list)", instance_, result);
        }
        return false;
    }

    const std::array<int64_t, 2> preferred_formats = {
        GL_SRGB8_ALPHA8,
        GL_RGBA8,
    };

    quad_swapchain_format_ = formats.front();
    for (const auto preferred : preferred_formats) {
        for (const auto available : formats) {
            if (available == preferred) {
                quad_swapchain_format_ = available;
                break;
            }
        }
        if (quad_swapchain_format_ == preferred) {
            break;
        }
    }

    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    create_info.format = quad_swapchain_format_;
    create_info.sampleCount = 1;
    create_info.width = quad_width_;
    create_info.height = quad_height_;
    create_info.faceCount = 1;
    create_info.arraySize = 1;
    create_info.mipCount = 1;

    result = xrCreateSwapchain(session_, &create_info, &quad_swapchain_);
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrCreateSwapchain(quad)", instance_, result);
        }
        return false;
    }

    uint32_t image_count = 0;
    result = xrEnumerateSwapchainImages(quad_swapchain_, 0, &image_count, nullptr);
    if (XR_FAILED(result) || image_count == 0) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateSwapchainImages(count)", instance_, result);
        }
        return false;
    }

    quad_images_.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    result = xrEnumerateSwapchainImages(
        quad_swapchain_,
        image_count,
        &image_count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(quad_images_.data()));
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateSwapchainImages(list)", instance_, result);
        }
        return false;
    }

    quad_framebuffers_.resize(image_count, 0);
    glGenFramebuffers(static_cast<GLsizei>(quad_framebuffers_.size()), quad_framebuffers_.data());

    for (std::size_t i = 0; i < quad_images_.size(); ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, quad_framebuffers_[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, quad_images_[i].image, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            if (error != nullptr) {
                std::ostringstream oss;
                oss << "Quad framebuffer incomplete. status=0x" << std::hex << status;
                *error = oss.str();
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "Quad swapchain created. format=0x%llx images=%u size=%ux%u",
                        static_cast<unsigned long long>(quad_swapchain_format_),
                        image_count,
                        quad_width_,
                        quad_height_);
    return true;
}

void XrPoseRuntime::DestroyQuadSwapchain() {
    if (!quad_framebuffers_.empty()) {
        glDeleteFramebuffers(static_cast<GLsizei>(quad_framebuffers_.size()), quad_framebuffers_.data());
        quad_framebuffers_.clear();
    }
    quad_images_.clear();

    if (quad_swapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(quad_swapchain_);
        quad_swapchain_ = XR_NULL_HANDLE;
    }
}

bool XrPoseRuntime::CreateAndroidSurfaceSwapchain(std::string* error) {
    if (create_swapchain_android_surface_khr_ == nullptr || update_swapchain_fb_ == nullptr) {
        if (error != nullptr) {
            *error = "Android surface swapchain extensions are not initialized.";
        }
        return false;
    }

    JNIEnv* env = nullptr;
    app_->activity->vm->AttachCurrentThread(&env, nullptr);

    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create_info.createFlags = 0;
    create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    create_info.format = 0;
    create_info.sampleCount = 0;
    create_info.width = encoded_width_;
    create_info.height = encoded_height_;
    create_info.faceCount = 0;
    create_info.arraySize = 0;
    create_info.mipCount = 0;

    jobject surface = nullptr;
    XrResult result = create_swapchain_android_surface_khr_(session_, &create_info, &android_surface_swapchain_, &surface);
    if (XR_FAILED(result) || android_surface_swapchain_ == XR_NULL_HANDLE || surface == nullptr) {
        if (error != nullptr) {
            *error = MakeError("xrCreateSwapchainAndroidSurfaceKHR", instance_, result);
        }
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Android surface swapchain handle created.");

    XrSwapchainStateAndroidSurfaceDimensionsFB dimensions{
        XR_TYPE_SWAPCHAIN_STATE_ANDROID_SURFACE_DIMENSIONS_FB};
    dimensions.width = encoded_width_;
    dimensions.height = encoded_height_;
    result = update_swapchain_fb_(android_surface_swapchain_,
                                  reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&dimensions));
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrUpdateSwapchainFB(AndroidSurfaceDimensions)", instance_, result);
        }
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Android surface swapchain dimensions updated.");

    ANativeWindow* codec_window = ANativeWindow_fromSurface(env, surface);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "ANativeWindow_fromSurface returned %p", codec_window);
    if (!h264_decoder_.Initialize(codec_window)) {
        if (codec_window != nullptr) {
            ANativeWindow_release(codec_window);
        }
        if (error != nullptr) {
            *error = "Failed to initialize MediaCodec decoder output window.";
        }
        return false;
    }
    if (codec_window != nullptr) {
        ANativeWindow_release(codec_window);
    }

    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "Android surface swapchain created for encoded video %ux%u",
                        encoded_width_,
                        encoded_height_);
    return true;
}

void XrPoseRuntime::DestroyAndroidSurfaceSwapchain() {
    h264_decoder_.Shutdown();
    android_surface_obj_ = nullptr;

    if (android_surface_swapchain_ != XR_NULL_HANDLE) {
        xrDestroySwapchain(android_surface_swapchain_);
        android_surface_swapchain_ = XR_NULL_HANDLE;
    }
}

void XrPoseRuntime::PumpEncodedVideoDecoder() {
    EncodedVideoFrame frame{};
    while (encoded_video_receiver_.TryPopFrame(&frame)) {
        h264_decoder_.OnFrame(frame);
    }
    h264_decoder_.DrainOutput();

    std::uint32_t related_frame_id = 0;
    const std::uint32_t pending_requests = h264_decoder_.ConsumePendingControlRequests(&related_frame_id);
    if ((pending_requests & DecoderControlRequestCodecConfig) != 0) {
        SendControlMessage(vt::proto::ControlMessageType::RequestCodecConfig,
                           related_frame_id,
                           vt::proto::ControlMessageFlagUrgent);
    }
    if ((pending_requests & DecoderControlRequestKeyframe) != 0) {
        SendControlMessage(vt::proto::ControlMessageType::RequestKeyframe,
                           related_frame_id,
                           vt::proto::ControlMessageFlagUrgent);
    }

    if (h264_decoder_.IsConfigured() && !h264_decoder_.HasRenderedFrame() && !logged_waiting_for_decoded_frame_) {
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "H264 decoder configured and waiting for first rendered frame. queuedInputs=%llu",
                            static_cast<unsigned long long>(h264_decoder_.QueuedFrameCount()));
        logged_waiting_for_decoded_frame_ = true;
    }

    if (h264_decoder_.HasRenderedFrame() && !logged_surface_layer_active_) {
        std::string save_error;
        if (!SaveLastSuccessfulRuntimeConfig(app_, config_, &save_error)) {
            __android_log_print(ANDROID_LOG_WARN,
                                kLogTag,
                                "Failed to save last successful runtime config: %s",
                                save_error.c_str());
        }
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "First decoded frame is available. OpenXR quad layer will use Android surface swapchain.");
        logged_surface_layer_active_ = true;
    }
}

void XrPoseRuntime::PollEvents(bool* exit_render_loop, bool* request_restart) {
    *exit_render_loop = false;
    *request_restart = false;

    XrEventDataBuffer event_buffer{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event_buffer) == XR_SUCCESS) {
        switch (event_buffer.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto* changed_event =
                    reinterpret_cast<const XrEventDataSessionStateChanged*>(&event_buffer);
                HandleSessionStateChanged(*changed_event, exit_render_loop, request_restart);
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                __android_log_print(ANDROID_LOG_WARN, kLogTag, "OpenXR instance loss pending.");
                *exit_render_loop = true;
                *request_restart = true;
                break;
            default:
                __android_log_print(ANDROID_LOG_VERBOSE, kLogTag, "Ignoring OpenXR event type=%d", event_buffer.type);
                break;
        }

        event_buffer = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void XrPoseRuntime::HandleSessionStateChanged(const XrEventDataSessionStateChanged& changed_event,
                                              bool* exit_render_loop,
                                              bool* request_restart) {
    session_state_ = changed_event.state;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "Session state changed -> %d", session_state_);

    switch (session_state_) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
            begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            const XrResult begin_result = xrBeginSession(session_, &begin_info);
            if (XR_SUCCEEDED(begin_result)) {
                session_running_ = true;
            } else {
                __android_log_print(ANDROID_LOG_ERROR,
                                    kLogTag,
                                    "xrBeginSession failed: %s",
                                    XrResultToString(instance_, begin_result).c_str());
            }
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            session_running_ = false;
            xrEndSession(session_);
            break;
        case XR_SESSION_STATE_EXITING:
            *exit_render_loop = true;
            *request_restart = false;
            break;
        case XR_SESSION_STATE_LOSS_PENDING:
            *exit_render_loop = true;
            *request_restart = true;
            break;
        default:
            break;
    }
}

bool XrPoseRuntime::SupportsExtension(const char* extension_name) const {
    for (const auto& extension : available_extensions_) {
        if (extension == extension_name) {
            return true;
        }
    }
    return false;
}

void XrPoseRuntime::TickBackgroundWork() {
    PumpEncodedVideoDecoder();
}

void XrPoseRuntime::RunFrame() {
    if (!session_running_) {
        return;
    }

    XrFrameWaitInfo frame_wait_info{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frame_state{XR_TYPE_FRAME_STATE};
    const XrResult wait_result = xrWaitFrame(session_, &frame_wait_info, &frame_state);
    if (XR_FAILED(wait_result)) {
        __android_log_print(
            ANDROID_LOG_WARN, kLogTag, "xrWaitFrame failed: %s", XrResultToString(instance_, wait_result).c_str());
        return;
    }

    XrFrameBeginInfo frame_begin_info{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult begin_result = xrBeginFrame(session_, &frame_begin_info);
    if (XR_FAILED(begin_result)) {
        __android_log_print(
            ANDROID_LOG_WARN, kLogTag, "xrBeginFrame failed: %s", XrResultToString(instance_, begin_result).c_str());
        return;
    }

    PumpEncodedVideoDecoder();

    std::vector<XrCompositionLayerBaseHeader*> layers;
    XrCompositionLayerQuad quad_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerImageLayoutFB image_layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
    const bool should_vertical_flip_encoded_video =
        (h264_decoder_.CurrentStreamFlags() & vt::proto::VideoFrameFlagVerticalFlip) != 0;
    if (composition_layer_image_layout_enabled_ && should_vertical_flip_encoded_video) {
        image_layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
        quad_layer.next = &image_layout;
    }
    if (h264_decoder_.HasRenderedFrame() && android_surface_swapchain_ != XR_NULL_HANDLE) {
        quad_layer.space = app_space_;
        quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad_layer.pose.orientation.w = 1.0f;
        quad_layer.pose.position.x = 0.0f;
        quad_layer.pose.position.y = 0.0f;
        quad_layer.pose.position.z = -1.2f;
        quad_layer.size.width = 1.6f;
        quad_layer.size.height = 0.9f;
        quad_layer.subImage.swapchain = android_surface_swapchain_;
        quad_layer.subImage.imageRect.offset = {0, 0};
        quad_layer.subImage.imageRect.extent = {static_cast<int32_t>(encoded_width_), static_cast<int32_t>(encoded_height_)};
        quad_layer.subImage.imageArrayIndex = 0;
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad_layer));
    } else if (RenderQuadLayer(frame_state.predictedDisplayTime, &quad_layer)) {
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad_layer));
    }

    SendHeadPose(frame_state.predictedDisplayTime);

    XrFrameEndInfo frame_end_info{XR_TYPE_FRAME_END_INFO};
    frame_end_info.displayTime = frame_state.predictedDisplayTime;
    frame_end_info.environmentBlendMode = blend_mode_;
    frame_end_info.layerCount = static_cast<uint32_t>(layers.size());
    frame_end_info.layers = layers.empty() ? nullptr : layers.data();

    const XrResult end_result = xrEndFrame(session_, &frame_end_info);
    if (XR_FAILED(end_result)) {
        __android_log_print(
            ANDROID_LOG_WARN, kLogTag, "xrEndFrame failed: %s", XrResultToString(instance_, end_result).c_str());
    }
}

bool XrPoseRuntime::RenderQuadLayer(XrTime predicted_display_time, XrCompositionLayerQuad* layer) {
    if (quad_swapchain_ == XR_NULL_HANDLE || layer == nullptr || quad_images_.empty()) {
        return false;
    }

    uint32_t image_index = 0;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(quad_swapchain_, &acquire_info, &image_index);
    if (XR_FAILED(result)) {
        __android_log_print(
            ANDROID_LOG_WARN, kLogTag, "xrAcquireSwapchainImage failed: %s", XrResultToString(instance_, result).c_str());
        return false;
    }

    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(quad_swapchain_, &wait_info);
    if (XR_FAILED(result)) {
        __android_log_print(
            ANDROID_LOG_WARN, kLogTag, "xrWaitSwapchainImage failed: %s", XrResultToString(instance_, result).c_str());
        return false;
    }

    VideoFrame video_frame{};
    const bool has_video_frame = video_receiver_.TryConsumeLatestFrame(&video_frame);
    if (has_video_frame &&
        video_frame.pixel_format == vt::proto::VideoPixelFormat::Rgba8888 &&
        video_frame.width == quad_width_ &&
        video_frame.height == quad_height_ &&
        !video_frame.pixels.empty()) {
        glBindTexture(GL_TEXTURE_2D, quad_images_[image_index].image);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D,
                        0,
                        0,
                        0,
                        static_cast<GLsizei>(quad_width_),
                        static_cast<GLsizei>(quad_height_),
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        video_frame.pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        const float t = static_cast<float>(frames_sent_ % 600) / 600.0f;
        const float red = 0.1f + 0.4f * std::sin(t * 6.2831853f);
        const float green = 0.3f + 0.3f * std::sin((t + 0.33f) * 6.2831853f);
        const float blue = 0.6f + 0.2f * std::sin((t + 0.66f) * 6.2831853f);

        glBindFramebuffer(GL_FRAMEBUFFER, quad_framebuffers_[image_index]);
        glViewport(0, 0, static_cast<GLsizei>(quad_width_), static_cast<GLsizei>(quad_height_));
        glClearColor(red, green, blue, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(quad_swapchain_, &release_info);

    layer->space = app_space_;
    layer->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer->pose.orientation.w = 1.0f;
    layer->pose.position.x = 0.0f;
    layer->pose.position.y = 0.0f;
    layer->pose.position.z = -1.2f;
    layer->size.width = 1.6f;
    layer->size.height = 0.9f;
    layer->subImage.swapchain = quad_swapchain_;
    layer->subImage.imageRect.offset = {0, 0};
    layer->subImage.imageRect.extent = {static_cast<int32_t>(quad_width_), static_cast<int32_t>(quad_height_)};
    layer->subImage.imageArrayIndex = 0;
    (void)predicted_display_time;
    return true;
}

bool XrPoseRuntime::SendHeadPose(XrTime predicted_display_time) {
    const vt::proto::PosePayload pose = BuildPosePayload(predicted_display_time);

    vt::proto::PacketHeader header{};
    header.type = static_cast<std::uint16_t>(vt::proto::PacketType::Pose);
    header.payload_size = sizeof(vt::proto::PosePayload);
    header.sequence = pose_sequence_++;
    header.timestamp_us = vt::proto::NowMicroseconds();

    const bool sent = pose_sender_.SendPose(header, pose);
    if (sent) {
        frames_sent_ += 1;
        if (frames_sent_ <= 5 || (frames_sent_ % 120) == 0) {
            __android_log_print(ANDROID_LOG_INFO,
                                kLogTag,
                                "Pose sent seq=%u pos=(%.3f, %.3f, %.3f) flags=0x%x",
                                header.sequence,
                                pose.position_m.x,
                                pose.position_m.y,
                                pose.position_m.z,
                                pose.tracking_flags);
        }
    } else {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "Failed to send pose packet seq=%u", header.sequence);
    }

    return sent;
}

bool XrPoseRuntime::SendControlMessage(vt::proto::ControlMessageType message_type,
                                       std::uint32_t related_frame_id,
                                       std::uint16_t flags,
                                       std::uint32_t value0,
                                       std::uint32_t value1) {
    vt::proto::PacketHeader header{};
    header.type = static_cast<std::uint16_t>(vt::proto::PacketType::Control);
    header.payload_size = sizeof(vt::proto::ControlPayload);
    header.sequence = control_sequence_++;
    header.timestamp_us = vt::proto::NowMicroseconds();

    vt::proto::ControlPayload payload{};
    payload.message_type = static_cast<std::uint16_t>(message_type);
    payload.flags = flags;
    payload.request_id = header.sequence;
    payload.related_frame_id = related_frame_id;
    payload.value0 = value0;
    payload.value1 = value1;

    const bool sent = control_sender_.SendControl(header, payload);
    if (sent) {
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "Control sent seq=%u type=%s relatedFrame=%u flags=0x%x",
                            header.sequence,
                            vt::proto::ControlMessageTypeName(message_type),
                            related_frame_id,
                            flags);
    } else {
        __android_log_print(ANDROID_LOG_WARN,
                            kLogTag,
                            "Failed to send control seq=%u type=%s",
                            header.sequence,
                            vt::proto::ControlMessageTypeName(message_type));
    }
    return sent;
}

vt::proto::PosePayload XrPoseRuntime::BuildPosePayload(XrTime predicted_display_time) const {
    vt::proto::PosePayload payload{};
    payload.orientation.w = 1.0f;

    if (head_space_ == XR_NULL_HANDLE || app_space_ == XR_NULL_HANDLE) {
        return payload;
    }

    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    const XrResult locate_result = xrLocateSpace(head_space_, app_space_, predicted_display_time, &location);
    if (XR_FAILED(locate_result)) {
        __android_log_print(ANDROID_LOG_WARN,
                            kLogTag,
                            "xrLocateSpace failed: %s",
                            XrResultToString(instance_, locate_result).c_str());
        return payload;
    }

    payload.position_m.x = location.pose.position.x;
    payload.position_m.y = location.pose.position.y;
    payload.position_m.z = location.pose.position.z;
    payload.orientation.x = location.pose.orientation.x;
    payload.orientation.y = location.pose.orientation.y;
    payload.orientation.z = location.pose.orientation.z;
    payload.orientation.w = location.pose.orientation.w;
    payload.tracking_flags = ToTrackingFlags(location.locationFlags);
    return payload;
}

}  // namespace vt::android
