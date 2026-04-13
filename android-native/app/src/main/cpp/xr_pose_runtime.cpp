#include "xr_pose_runtime.h"

#include "control_protocol.h"
#include "runtime_config_store.h"
#include "time_sync.h"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <android/surface_texture_jni.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

#include <GLES2/gl2ext.h>

namespace vt::android {
namespace {

constexpr const char* kLogTag = "videotest-native";
constexpr XrReferenceSpaceType kAppSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
constexpr XrReferenceSpaceType kHeadSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
constexpr bool kProjectionMonoDiagnosticUseQuadSwapchain = false;

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

std::string GlInfoLog(GLuint object, bool shader) {
    GLint log_length = 0;
    if (shader) {
        glGetShaderiv(object, GL_INFO_LOG_LENGTH, &log_length);
    } else {
        glGetProgramiv(object, GL_INFO_LOG_LENGTH, &log_length);
    }
    if (log_length <= 1) {
        return {};
    }

    std::vector<char> log(static_cast<std::size_t>(log_length), '\0');
    GLsizei written = 0;
    if (shader) {
        glGetShaderInfoLog(object, log_length, &written, log.data());
    } else {
        glGetProgramInfoLog(object, log_length, &written, log.data());
    }
    return std::string(log.data(), static_cast<std::size_t>(std::max<GLsizei>(written, 0)));
}

GLuint CompileShader(GLenum type, const char* source, std::string* error) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        if (error != nullptr) {
            *error = "GL shader compile failed: " + GlInfoLog(shader, true);
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint LinkProgram(const char* vertex_source, const char* fragment_source, std::string* error) {
    const GLuint vertex_shader = CompileShader(GL_VERTEX_SHADER, vertex_source, error);
    if (vertex_shader == 0) {
        return 0;
    }
    const GLuint fragment_shader = CompileShader(GL_FRAGMENT_SHADER, fragment_source, error);
    if (fragment_shader == 0) {
        glDeleteShader(vertex_shader);
        return 0;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        if (error != nullptr) {
            *error = "GL program link failed: " + GlInfoLog(program, false);
        }
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

bool ClearJniException(JNIEnv* env, const char* step, std::string* error) {
    if (env == nullptr || !env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    if (error != nullptr) {
        *error = std::string(step) + " raised a JNI exception.";
    }
    return true;
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
    if (!InitializePrimaryStereoViews(error)) {
        return false;
    }
    if (!CreateReferenceSpaces(error)) {
        return false;
    }
    if (!CreateQuadSwapchain(error)) {
        return false;
    }
    if (UsesDecodedVideoGlProjection()) {
        if (!CreateDecodedVideoGlOutput(error)) {
            return false;
        }
    } else if (!CreateAndroidSurfaceSwapchain(error)) {
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
    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "Decoded video display mode: %s",
                        DecodedVideoDisplayModeName(config_.display_mode));

    return true;
}

void XrPoseRuntime::Shutdown() {
    DestroyDecodedVideoGlOutput();
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

bool XrPoseRuntime::InitializePrimaryStereoViews(std::string* error) {
    uint32_t view_count = 0;
    XrResult result = xrEnumerateViewConfigurationViews(
        instance_, system_id_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);
    if (XR_FAILED(result) || view_count == 0) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateViewConfigurationViews(count)", instance_, result);
        }
        return false;
    }

    primary_stereo_view_configs_.assign(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = xrEnumerateViewConfigurationViews(instance_,
                                               system_id_,
                                               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               view_count,
                                               &view_count,
                                               primary_stereo_view_configs_.data());
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateViewConfigurationViews(list)", instance_, result);
        }
        return false;
    }

    primary_stereo_views_.assign(view_count, {XR_TYPE_VIEW});
    for (auto& view : primary_stereo_views_) {
        view = {XR_TYPE_VIEW};
    }

    for (std::size_t index = 0; index < primary_stereo_view_configs_.size(); ++index) {
        const auto& view = primary_stereo_view_configs_[index];
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "Primary stereo view[%zu]: recommended=%ux%u max=%ux%u samples=%u",
                            index,
                            view.recommendedImageRectWidth,
                            view.recommendedImageRectHeight,
                            view.maxImageRectWidth,
                            view.maxImageRectHeight,
                            view.recommendedSwapchainSampleCount);
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

std::uint32_t XrPoseRuntime::TargetDecodedVideoGlViewCount() const noexcept {
    return config_.display_mode == DecodedVideoDisplayMode::ProjectionStereo ? 2u : 1u;
}

bool XrPoseRuntime::HasDecodedVideoForGlView(std::uint32_t view_index) const noexcept {
    return view_index < decoded_video_gl_view_count_ &&
           view_index < decoded_video_gl_views_.size() &&
           decoded_video_gl_views_[view_index].decoder.HasRenderedFrame();
}

std::uint16_t XrPoseRuntime::CurrentDecodedGlStreamFlags(std::uint32_t view_index) const noexcept {
    if (view_index >= decoded_video_gl_view_count_ || view_index >= decoded_video_gl_views_.size()) {
        return 0;
    }

    return decoded_video_gl_views_[view_index].decoder.CurrentStreamFlags();
}

bool XrPoseRuntime::CreateDecodedVideoGlOutput(std::string* error) {
    uint32_t format_count = 0;
    XrResult result = xrEnumerateSwapchainFormats(session_, 0, &format_count, nullptr);
    if (XR_FAILED(result) || format_count == 0) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateSwapchainFormats(decoded-video count)", instance_, result);
        }
        return false;
    }

    std::vector<int64_t> formats(format_count);
    result = xrEnumerateSwapchainFormats(session_, format_count, &format_count, formats.data());
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateSwapchainFormats(decoded-video list)", instance_, result);
        }
        return false;
    }

    const std::array<int64_t, 2> preferred_formats = {
        GL_SRGB8_ALPHA8,
        GL_RGBA8,
    };

    decoded_video_gl_swapchain_format_ = formats.front();
    for (const auto preferred : preferred_formats) {
        for (const auto available : formats) {
            if (available == preferred) {
                decoded_video_gl_swapchain_format_ = available;
                break;
            }
        }
        if (decoded_video_gl_swapchain_format_ == preferred) {
            break;
        }
    }

    constexpr const char* kBlitVertexShader = R"(
attribute vec2 aPosition;
attribute vec2 aTexCoord;
uniform mat4 uTexTransform;
uniform float uVerticalFlip;
varying vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vec2 texCoord = aTexCoord;
    if (uVerticalFlip > 0.5) {
        texCoord.y = 1.0 - texCoord.y;
    }
    vec4 transformed = uTexTransform * vec4(texCoord, 0.0, 1.0);
    vTexCoord = transformed.xy;
}
)";

    constexpr const char* kBlitFragmentShader = R"(
#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES uTexture;
varying vec2 vTexCoord;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord);
}
)";

    decoded_blit_program_ = LinkProgram(kBlitVertexShader, kBlitFragmentShader, error);
    if (decoded_blit_program_ == 0) {
        DestroyDecodedVideoGlOutput();
        return false;
    }
    decoded_blit_position_attrib_ = glGetAttribLocation(decoded_blit_program_, "aPosition");
    decoded_blit_texcoord_attrib_ = glGetAttribLocation(decoded_blit_program_, "aTexCoord");
    decoded_blit_texture_uniform_ = glGetUniformLocation(decoded_blit_program_, "uTexture");
    decoded_blit_transform_uniform_ = glGetUniformLocation(decoded_blit_program_, "uTexTransform");
    decoded_blit_vertical_flip_uniform_ = glGetUniformLocation(decoded_blit_program_, "uVerticalFlip");
    if (decoded_blit_position_attrib_ < 0 || decoded_blit_texcoord_attrib_ < 0 ||
        decoded_blit_texture_uniform_ < 0 || decoded_blit_transform_uniform_ < 0 ||
        decoded_blit_vertical_flip_uniform_ < 0) {
        if (error != nullptr) {
            *error = "Decoded-video GL blit shader is missing required attributes or uniforms.";
        }
        DestroyDecodedVideoGlOutput();
        return false;
    }

    constexpr GLfloat kVertices[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };
    glGenBuffers(1, &decoded_blit_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, decoded_blit_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    decoded_video_gl_view_count_ = TargetDecodedVideoGlViewCount();
    for (std::uint32_t view_index = 0; view_index < decoded_video_gl_view_count_; ++view_index) {
        if (!CreateDecodedVideoGlOutputView(view_index, error)) {
            DestroyDecodedVideoGlOutput();
            return false;
        }
    }

    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "Decoded-video GL output created. views=%u swapchainFormat=0x%llx size=%ux%u",
                        decoded_video_gl_view_count_,
                        static_cast<unsigned long long>(decoded_video_gl_swapchain_format_),
                        encoded_width_,
                        encoded_height_);
    return true;
}

bool XrPoseRuntime::CreateDecodedVideoGlOutputView(std::uint32_t view_index, std::string* error) {
    if (view_index >= decoded_video_gl_views_.size()) {
        if (error != nullptr) {
            *error = "Decoded-video GL view index is out of range.";
        }
        return false;
    }

    auto cleanup = [&]() {
        DestroyDecodedVideoGlOutputView(view_index);
        return false;
    };

    auto& view_output = decoded_video_gl_views_[view_index];

    XrSwapchainCreateInfo create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    create_info.format = decoded_video_gl_swapchain_format_;
    create_info.sampleCount = 1;
    create_info.width = encoded_width_;
    create_info.height = encoded_height_;
    create_info.faceCount = 1;
    create_info.arraySize = 1;
    create_info.mipCount = 1;

    XrResult result = xrCreateSwapchain(session_, &create_info, &view_output.swapchain);
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrCreateSwapchain(decoded-video GL view)", instance_, result);
        }
        return cleanup();
    }

    uint32_t image_count = 0;
    result = xrEnumerateSwapchainImages(view_output.swapchain, 0, &image_count, nullptr);
    if (XR_FAILED(result) || image_count == 0) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateSwapchainImages(decoded-video view count)", instance_, result);
        }
        return cleanup();
    }

    view_output.images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    result = xrEnumerateSwapchainImages(view_output.swapchain,
                                        image_count,
                                        &image_count,
                                        reinterpret_cast<XrSwapchainImageBaseHeader*>(view_output.images.data()));
    if (XR_FAILED(result)) {
        if (error != nullptr) {
            *error = MakeError("xrEnumerateSwapchainImages(decoded-video view list)", instance_, result);
        }
        return cleanup();
    }

    view_output.framebuffers.resize(image_count, 0);
    glGenFramebuffers(static_cast<GLsizei>(view_output.framebuffers.size()), view_output.framebuffers.data());
    for (std::size_t i = 0; i < view_output.images.size(); ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, view_output.framebuffers[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, view_output.images[i].image, 0);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            if (error != nullptr) {
                std::ostringstream oss;
                oss << "Decoded-video GL framebuffer incomplete for view " << view_index << ". status=0x" << std::hex
                    << status;
                *error = oss.str();
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            return cleanup();
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    glGenTextures(1, &view_output.external_texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, view_output.external_texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    JNIEnv* env = nullptr;
    if (app_ == nullptr || app_->activity == nullptr || app_->activity->vm == nullptr ||
        app_->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) {
        if (error != nullptr) {
            *error = "Failed to attach JNI thread for SurfaceTexture creation.";
        }
        return cleanup();
    }

    jclass surface_texture_class = env->FindClass("android/graphics/SurfaceTexture");
    if (ClearJniException(env, "FindClass(SurfaceTexture)", error) || surface_texture_class == nullptr) {
        if (error != nullptr && error->empty()) {
            *error = "Failed to find android.graphics.SurfaceTexture.";
        }
        return cleanup();
    }

    jmethodID constructor = env->GetMethodID(surface_texture_class, "<init>", "(I)V");
    if (ClearJniException(env, "GetMethodID(SurfaceTexture.<init>)", error) || constructor == nullptr) {
        if (error != nullptr && error->empty()) {
            *error = "Failed to find SurfaceTexture(int) constructor.";
        }
        env->DeleteLocalRef(surface_texture_class);
        return cleanup();
    }

    jobject surface_texture = env->NewObject(surface_texture_class, constructor, static_cast<jint>(view_output.external_texture));
    if (ClearJniException(env, "NewObject(SurfaceTexture)", error) || surface_texture == nullptr) {
        if (error != nullptr && error->empty()) {
            *error = "Failed to create SurfaceTexture.";
        }
        env->DeleteLocalRef(surface_texture_class);
        return cleanup();
    }

    jmethodID set_default_buffer_size = env->GetMethodID(surface_texture_class, "setDefaultBufferSize", "(II)V");
    if (ClearJniException(env, "GetMethodID(setDefaultBufferSize)", nullptr)) {
        set_default_buffer_size = nullptr;
    }
    if (set_default_buffer_size != nullptr) {
        env->CallVoidMethod(surface_texture,
                            set_default_buffer_size,
                            static_cast<jint>(encoded_width_),
                            static_cast<jint>(encoded_height_));
        if (ClearJniException(env, "SurfaceTexture.setDefaultBufferSize", error)) {
            env->DeleteLocalRef(surface_texture);
            env->DeleteLocalRef(surface_texture_class);
            return cleanup();
        }
    }

    view_output.surface_texture_obj = env->NewGlobalRef(surface_texture);
    env->DeleteLocalRef(surface_texture);
    env->DeleteLocalRef(surface_texture_class);
    if (ClearJniException(env, "NewGlobalRef(SurfaceTexture)", error) || view_output.surface_texture_obj == nullptr) {
        if (error != nullptr && error->empty()) {
            *error = "Failed to keep a global SurfaceTexture reference.";
        }
        return cleanup();
    }

    view_output.surface_texture = ASurfaceTexture_fromSurfaceTexture(env, view_output.surface_texture_obj);
    if (view_output.surface_texture == nullptr) {
        if (error != nullptr) {
            *error = "ASurfaceTexture_fromSurfaceTexture returned null.";
        }
        return cleanup();
    }

    ANativeWindow* codec_window = ASurfaceTexture_acquireANativeWindow(view_output.surface_texture);
    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "ASurfaceTexture_acquireANativeWindow view=%u returned %p",
                        view_index,
                        codec_window);
    if (!view_output.decoder.Initialize(codec_window)) {
        if (codec_window != nullptr) {
            ANativeWindow_release(codec_window);
        }
        if (error != nullptr) {
            *error = "Failed to initialize MediaCodec decoder with SurfaceTexture output window.";
        }
        return cleanup();
    }
    if (codec_window != nullptr) {
        ANativeWindow_release(codec_window);
    }

    view_output.logged_waiting_for_first_frame = false;
    view_output.logged_first_frame_ready = false;

    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "Decoded-video GL view created. view=%u images=%u size=%ux%u texture=%u",
                        view_index,
                        image_count,
                        encoded_width_,
                        encoded_height_,
                        view_output.external_texture);
    return true;
}

void XrPoseRuntime::DestroyDecodedVideoGlOutput() {
    for (std::uint32_t view_index = 0; view_index < decoded_video_gl_views_.size(); ++view_index) {
        DestroyDecodedVideoGlOutputView(view_index);
    }
    decoded_video_gl_view_count_ = 0;

    if (decoded_blit_vbo_ != 0) {
        glDeleteBuffers(1, &decoded_blit_vbo_);
        decoded_blit_vbo_ = 0;
    }
    if (decoded_blit_program_ != 0) {
        glDeleteProgram(decoded_blit_program_);
        decoded_blit_program_ = 0;
    }
    decoded_blit_position_attrib_ = -1;
    decoded_blit_texcoord_attrib_ = -1;
    decoded_blit_texture_uniform_ = -1;
    decoded_blit_transform_uniform_ = -1;
    decoded_blit_vertical_flip_uniform_ = -1;
}

void XrPoseRuntime::DestroyDecodedVideoGlOutputView(std::uint32_t view_index) {
    if (view_index >= decoded_video_gl_views_.size()) {
        return;
    }

    auto& view_output = decoded_video_gl_views_[view_index];
    const bool had_output =
        view_output.swapchain != XR_NULL_HANDLE || view_output.surface_texture != nullptr ||
        view_output.surface_texture_obj != nullptr || view_output.external_texture != 0;
    if (had_output) {
        view_output.decoder.Shutdown();
    }

    if (view_output.surface_texture != nullptr) {
        ASurfaceTexture_release(view_output.surface_texture);
        view_output.surface_texture = nullptr;
    }

    if (view_output.surface_texture_obj != nullptr && app_ != nullptr &&
        app_->activity != nullptr && app_->activity->vm != nullptr) {
        JNIEnv* env = nullptr;
        if (app_->activity->vm->AttachCurrentThread(&env, nullptr) == JNI_OK && env != nullptr) {
            env->DeleteGlobalRef(view_output.surface_texture_obj);
        }
        view_output.surface_texture_obj = nullptr;
    }

    if (view_output.external_texture != 0) {
        glDeleteTextures(1, &view_output.external_texture);
        view_output.external_texture = 0;
    }

    if (!view_output.framebuffers.empty()) {
        glDeleteFramebuffers(static_cast<GLsizei>(view_output.framebuffers.size()), view_output.framebuffers.data());
        view_output.framebuffers.clear();
    }
    view_output.images.clear();

    if (view_output.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(view_output.swapchain);
        view_output.swapchain = XR_NULL_HANDLE;
    }

    view_output.logged_waiting_for_first_frame = false;
    view_output.logged_first_frame_ready = false;
}

bool XrPoseRuntime::RenderDecodedVideoToGlSwapchain(std::uint32_t view_index, bool vertical_flip) {
    if (view_index >= decoded_video_gl_view_count_ || view_index >= decoded_video_gl_views_.size()) {
        return false;
    }

    auto& view_output = decoded_video_gl_views_[view_index];
    if (view_output.surface_texture == nullptr || view_output.swapchain == XR_NULL_HANDLE ||
        view_output.images.empty() || view_output.framebuffers.empty() ||
        decoded_blit_program_ == 0 || decoded_blit_vbo_ == 0 || view_output.external_texture == 0) {
        return false;
    }

    const int update_result = ASurfaceTexture_updateTexImage(view_output.surface_texture);
    if (update_result != 0) {
        __android_log_print(ANDROID_LOG_WARN,
                            kLogTag,
                            "ASurfaceTexture_updateTexImage failed for view=%u: %d",
                            view_index,
                            update_result);
        return false;
    }

    float texture_transform[16]{};
    ASurfaceTexture_getTransformMatrix(view_output.surface_texture, texture_transform);

    uint32_t image_index = 0;
    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(view_output.swapchain, &acquire_info, &image_index);
    if (XR_FAILED(result) || image_index >= view_output.framebuffers.size()) {
        __android_log_print(ANDROID_LOG_WARN,
                            kLogTag,
                            "xrAcquireSwapchainImage(decoded-video GL view=%u) failed: %s index=%u",
                            view_index,
                            XrResultToString(instance_, result).c_str(),
                            image_index);
        return false;
    }

    XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait_info.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(view_output.swapchain, &wait_info);
    if (XR_FAILED(result)) {
        __android_log_print(ANDROID_LOG_WARN,
                            kLogTag,
                            "xrWaitSwapchainImage(decoded-video GL view=%u) failed: %s",
                            view_index,
                            XrResultToString(instance_, result).c_str());
        XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(view_output.swapchain, &release_info);
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, view_output.framebuffers[image_index]);
    glViewport(0, 0, static_cast<GLsizei>(encoded_width_), static_cast<GLsizei>(encoded_height_));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(decoded_blit_program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, view_output.external_texture);
    glUniform1i(decoded_blit_texture_uniform_, 0);
    glUniformMatrix4fv(decoded_blit_transform_uniform_, 1, GL_FALSE, texture_transform);
    glUniform1f(decoded_blit_vertical_flip_uniform_, vertical_flip ? 1.0f : 0.0f);
    glBindBuffer(GL_ARRAY_BUFFER, decoded_blit_vbo_);
    glEnableVertexAttribArray(static_cast<GLuint>(decoded_blit_position_attrib_));
    glEnableVertexAttribArray(static_cast<GLuint>(decoded_blit_texcoord_attrib_));
    glVertexAttribPointer(static_cast<GLuint>(decoded_blit_position_attrib_),
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          4 * static_cast<GLsizei>(sizeof(GLfloat)),
                          reinterpret_cast<const void*>(0));
    glVertexAttribPointer(static_cast<GLuint>(decoded_blit_texcoord_attrib_),
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          4 * static_cast<GLsizei>(sizeof(GLfloat)),
                          reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(static_cast<GLuint>(decoded_blit_texcoord_attrib_));
    glDisableVertexAttribArray(static_cast<GLuint>(decoded_blit_position_attrib_));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(view_output.swapchain, &release_info);
    return true;
}

void XrPoseRuntime::PumpEncodedVideoDecoder() {
    EncodedVideoFrame frame{};
    while (encoded_video_receiver_.TryPopFrame(&frame)) {
        if (!UsesDecodedVideoGlProjection()) {
            h264_decoder_.OnFrame(frame);
            continue;
        }

        std::uint32_t target_view_index = 0;
        if (decoded_video_gl_view_count_ > 1 &&
            frame.stereo.view_count > 1 &&
            frame.stereo.view_id < decoded_video_gl_view_count_) {
            target_view_index = frame.stereo.view_id;
        }
        decoded_video_gl_views_[target_view_index].decoder.OnFrame(frame);
    }

    if (!UsesDecodedVideoGlProjection()) {
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
        return;
    }

    std::uint32_t codec_config_related_frame_id = 0;
    std::uint32_t keyframe_related_frame_id = 0;
    bool request_codec_config = false;
    bool request_keyframe = false;
    bool has_any_rendered_frame = false;
    bool saved_runtime_config = false;

    for (std::uint32_t view_index = 0; view_index < decoded_video_gl_view_count_; ++view_index) {
        auto& view_output = decoded_video_gl_views_[view_index];
        view_output.decoder.DrainOutput();

        std::uint32_t related_frame_id = 0;
        const std::uint32_t pending_requests = view_output.decoder.ConsumePendingControlRequests(&related_frame_id);
        if ((pending_requests & DecoderControlRequestCodecConfig) != 0) {
            request_codec_config = true;
            codec_config_related_frame_id = std::max(codec_config_related_frame_id, related_frame_id);
        }
        if ((pending_requests & DecoderControlRequestKeyframe) != 0) {
            request_keyframe = true;
            keyframe_related_frame_id = std::max(keyframe_related_frame_id, related_frame_id);
        }

        if (view_output.decoder.IsConfigured() &&
            !view_output.decoder.HasRenderedFrame() &&
            !view_output.logged_waiting_for_first_frame) {
            __android_log_print(ANDROID_LOG_INFO,
                                kLogTag,
                                "Decoded-video GL decoder view=%u configured and waiting for first rendered frame. queuedInputs=%llu",
                                view_index,
                                static_cast<unsigned long long>(view_output.decoder.QueuedFrameCount()));
            view_output.logged_waiting_for_first_frame = true;
        }

        if (view_output.decoder.HasRenderedFrame()) {
            has_any_rendered_frame = true;
            if (!view_output.logged_first_frame_ready) {
                if (!saved_runtime_config) {
                    std::string save_error;
                    if (!SaveLastSuccessfulRuntimeConfig(app_, config_, &save_error)) {
                        __android_log_print(ANDROID_LOG_WARN,
                                            kLogTag,
                                            "Failed to save last successful runtime config: %s",
                                            save_error.c_str());
                    }
                    saved_runtime_config = true;
                }
                __android_log_print(ANDROID_LOG_INFO,
                                    kLogTag,
                                    "First decoded frame is available for GL projection view=%u.",
                                    view_index);
                view_output.logged_first_frame_ready = true;
            }
        }
    }

    if (request_codec_config) {
        SendControlMessage(vt::proto::ControlMessageType::RequestCodecConfig,
                           codec_config_related_frame_id,
                           vt::proto::ControlMessageFlagUrgent);
    }
    if (request_keyframe) {
        SendControlMessage(vt::proto::ControlMessageType::RequestKeyframe,
                           keyframe_related_frame_id,
                           vt::proto::ControlMessageFlagUrgent);
    }

    if (has_any_rendered_frame && !logged_surface_layer_active_) {
        __android_log_print(ANDROID_LOG_INFO,
                            kLogTag,
                            "First decoded frame is available. OpenXR projection path will use decoded-video GL swapchain(s).");
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
    XrCompositionLayerQuad diagnostic_quad_layer{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection projection_layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerProjectionView> projection_views;
    XrCompositionLayerImageLayoutFB image_layout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
    const bool using_decoded_gl_projection = UsesDecodedVideoGlProjection();
    const bool should_vertical_flip_encoded_video =
        ((using_decoded_gl_projection ? CurrentDecodedGlStreamFlags(0) : h264_decoder_.CurrentStreamFlags()) &
         vt::proto::VideoFrameFlagVerticalFlip) != 0;
    if (!using_decoded_gl_projection &&
        composition_layer_image_layout_enabled_ &&
        should_vertical_flip_encoded_video) {
        image_layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
        quad_layer.next = &image_layout;
    }
    const bool has_primary_decoded_frame =
        using_decoded_gl_projection ? HasDecodedVideoForGlView(0) : h264_decoder_.HasRenderedFrame();
    const bool has_decoded_output =
        using_decoded_gl_projection ? decoded_video_gl_view_count_ > 0 : android_surface_swapchain_ != XR_NULL_HANDLE;

    if (has_primary_decoded_frame && has_decoded_output) {
        DecodedVideoDisplayMode display_mode = config_.display_mode;

        bool submitted_decoded_layer = false;
        if (display_mode == DecodedVideoDisplayMode::ProjectionStereo &&
            using_decoded_gl_projection &&
            decoded_video_gl_view_count_ >= 2) {
            const bool left_vertical_flip =
                (CurrentDecodedGlStreamFlags(0) & vt::proto::VideoFrameFlagVerticalFlip) != 0;
            const bool right_vertical_flip =
                (CurrentDecodedGlStreamFlags(1) & vt::proto::VideoFrameFlagVerticalFlip) != 0;
            const bool stereo_ready = HasDecodedVideoForGlView(0) && HasDecodedVideoForGlView(1);
            if (stereo_ready &&
                RenderDecodedVideoToGlSwapchain(0, left_vertical_flip) &&
                RenderDecodedVideoToGlSwapchain(1, right_vertical_flip) &&
                TryBuildDecodedGlStereoProjectionLayer(
                    frame_state.predictedDisplayTime, &projection_layer, &projection_views)) {
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection_layer));
                submitted_decoded_layer = true;
                if (!logged_projection_stereo_active_) {
                    __android_log_print(ANDROID_LOG_INFO,
                                        kLogTag,
                                        "Decoded video is now submitted as projection_stereo with dedicated left/right projection swapchains.");
                    logged_projection_stereo_active_ = true;
                }
            } else {
                if (!stereo_ready && !logged_projection_stereo_waiting_) {
                    __android_log_print(ANDROID_LOG_INFO,
                                        kLogTag,
                                        "projection_stereo is waiting for both decoded views; temporarily reusing the primary decoded view for both eyes.");
                    logged_projection_stereo_waiting_ = true;
                } else if (stereo_ready && !logged_projection_build_failure_) {
                    __android_log_print(ANDROID_LOG_WARN,
                                        kLogTag,
                                        "Failed to build projection_stereo layer; temporarily falling back to primary-eye mono projection.");
                    logged_projection_build_failure_ = true;
                }

                if (RenderDecodedVideoToGlSwapchain(0, left_vertical_flip) &&
                    TryBuildDecodedGlProjectionLayer(
                        frame_state.predictedDisplayTime, &projection_layer, &projection_views)) {
                    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection_layer));
                    submitted_decoded_layer = true;
                }
            }
        } else if (display_mode == DecodedVideoDisplayMode::ProjectionMono &&
            ((kProjectionMonoDiagnosticUseQuadSwapchain &&
              RenderQuadLayer(frame_state.predictedDisplayTime, &diagnostic_quad_layer) &&
              TryBuildProjectionLayerFromSwapchain(
                  frame_state.predictedDisplayTime, quad_swapchain_, quad_width_, quad_height_, &projection_layer, &projection_views)) ||
             (using_decoded_gl_projection &&
              RenderDecodedVideoToGlSwapchain(0, should_vertical_flip_encoded_video) &&
              TryBuildDecodedGlProjectionLayer(
                  frame_state.predictedDisplayTime, &projection_layer, &projection_views)) ||
             (!kProjectionMonoDiagnosticUseQuadSwapchain &&
              android_surface_swapchain_ != XR_NULL_HANDLE &&
              TryBuildDecodedProjectionLayer(
                  frame_state.predictedDisplayTime, &projection_layer, &projection_views)))) {
            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection_layer));
            submitted_decoded_layer = true;
            if (!logged_projection_layer_active_) {
                __android_log_print(ANDROID_LOG_INFO,
                                    kLogTag,
                                    "Decoded video is now submitted as projection_mono for a more immersive full-FOV presentation.");
                logged_projection_layer_active_ = true;
            }
        } else if (display_mode == DecodedVideoDisplayMode::ProjectionMono && !logged_projection_build_failure_) {
            __android_log_print(ANDROID_LOG_WARN,
                                kLogTag,
                                "Failed to build projection_mono layer; falling back to quad layer.");
            logged_projection_build_failure_ = true;
        }

        if (!submitted_decoded_layer) {
            if (android_surface_swapchain_ != XR_NULL_HANDLE) {
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
                quad_layer.subImage.imageRect.extent = {static_cast<int32_t>(encoded_width_),
                                                        static_cast<int32_t>(encoded_height_)};
                quad_layer.subImage.imageArrayIndex = 0;
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad_layer));
            } else if (RenderQuadLayer(frame_state.predictedDisplayTime, &quad_layer)) {
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad_layer));
            }
        }
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

bool XrPoseRuntime::TryBuildDecodedProjectionLayer(
    XrTime predicted_display_time,
    XrCompositionLayerProjection* layer,
    std::vector<XrCompositionLayerProjectionView>* projection_views) {
    return TryBuildProjectionLayerFromSwapchain(
        predicted_display_time, android_surface_swapchain_, encoded_width_, encoded_height_, layer, projection_views);
}

bool XrPoseRuntime::TryBuildDecodedGlProjectionLayer(
    XrTime predicted_display_time,
    XrCompositionLayerProjection* layer,
    std::vector<XrCompositionLayerProjectionView>* projection_views) {
    if (decoded_video_gl_view_count_ == 0 || decoded_video_gl_views_[0].swapchain == XR_NULL_HANDLE) {
        return false;
    }

    return TryBuildProjectionLayerFromSwapchain(predicted_display_time,
                                                decoded_video_gl_views_[0].swapchain,
                                                encoded_width_,
                                                encoded_height_,
                                                layer,
                                                projection_views);
}

bool XrPoseRuntime::TryBuildDecodedGlStereoProjectionLayer(
    XrTime predicted_display_time,
    XrCompositionLayerProjection* layer,
    std::vector<XrCompositionLayerProjectionView>* projection_views) {
    if (decoded_video_gl_view_count_ < 2 ||
        decoded_video_gl_views_[0].swapchain == XR_NULL_HANDLE ||
        decoded_video_gl_views_[1].swapchain == XR_NULL_HANDLE) {
        return false;
    }

    const XrSwapchain swapchains[2] = {
        decoded_video_gl_views_[0].swapchain,
        decoded_video_gl_views_[1].swapchain,
    };
    return TryBuildProjectionLayerFromSwapchains(predicted_display_time,
                                                 swapchains,
                                                 2,
                                                 encoded_width_,
                                                 encoded_height_,
                                                 layer,
                                                 projection_views);
}

bool XrPoseRuntime::TryBuildProjectionLayerFromSwapchain(
    XrTime predicted_display_time,
    XrSwapchain swapchain,
    std::uint32_t image_width,
    std::uint32_t image_height,
    XrCompositionLayerProjection* layer,
    std::vector<XrCompositionLayerProjectionView>* projection_views) {
    const XrSwapchain swapchains[] = {swapchain};
    return TryBuildProjectionLayerFromSwapchains(predicted_display_time,
                                                 swapchains,
                                                 1,
                                                 image_width,
                                                 image_height,
                                                 layer,
                                                 projection_views);
}

bool XrPoseRuntime::TryBuildProjectionLayerFromSwapchains(
    XrTime predicted_display_time,
    const XrSwapchain* swapchains,
    std::uint32_t swapchain_count,
    std::uint32_t image_width,
    std::uint32_t image_height,
    XrCompositionLayerProjection* layer,
    std::vector<XrCompositionLayerProjectionView>* projection_views) {
    if (layer == nullptr || projection_views == nullptr || swapchains == nullptr || swapchain_count == 0 ||
        primary_stereo_views_.empty() || app_space_ == XR_NULL_HANDLE) {
        return false;
    }

    XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
    locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate_info.displayTime = predicted_display_time;
    locate_info.space = app_space_;

    XrViewState view_state{XR_TYPE_VIEW_STATE};
    uint32_t output_view_count = 0;
    for (auto& view : primary_stereo_views_) {
        view = {XR_TYPE_VIEW};
    }

    const XrResult locate_result = xrLocateViews(session_,
                                                 &locate_info,
                                                 &view_state,
                                                 static_cast<uint32_t>(primary_stereo_views_.size()),
                                                 &output_view_count,
                                                 primary_stereo_views_.data());
    if (XR_FAILED(locate_result) || output_view_count == 0 || output_view_count > primary_stereo_views_.size()) {
        return false;
    }

    const XrViewStateFlags required_flags =
        XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    if ((view_state.viewStateFlags & required_flags) != required_flags) {
        return false;
    }

    if (swapchain_count != 1 && swapchain_count < output_view_count) {
        return false;
    }

    projection_views->clear();
    projection_views->resize(output_view_count);

    for (uint32_t index = 0; index < output_view_count; ++index) {
        const XrSwapchain target_swapchain = swapchains[swapchain_count == 1 ? 0 : index];
        if (target_swapchain == XR_NULL_HANDLE) {
            return false;
        }

        auto& projection_view = (*projection_views)[index];
        projection_view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        projection_view.pose = primary_stereo_views_[index].pose;
        projection_view.fov = primary_stereo_views_[index].fov;
        projection_view.subImage.swapchain = target_swapchain;
        projection_view.subImage.imageRect.offset = {0, 0};
        projection_view.subImage.imageRect.extent = {static_cast<int32_t>(image_width),
                                                     static_cast<int32_t>(image_height)};
        projection_view.subImage.imageArrayIndex = 0;
    }

    layer->space = app_space_;
    layer->viewCount = output_view_count;
    layer->views = projection_views->data();
    return true;
}

bool XrPoseRuntime::UsesDecodedVideoGlProjection() const noexcept {
    return !kProjectionMonoDiagnosticUseQuadSwapchain &&
           (config_.display_mode == DecodedVideoDisplayMode::ProjectionMono ||
            config_.display_mode == DecodedVideoDisplayMode::ProjectionStereo);
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
