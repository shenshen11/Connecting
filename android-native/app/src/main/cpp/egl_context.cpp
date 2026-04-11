#include "egl_context.h"

#include <android/log.h>

#include <sstream>

namespace vt::android {
namespace {

constexpr const char* kLogTag = "videotest-native";

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

const char* EglErrorToString(EGLint error) {
    switch (error) {
        case EGL_SUCCESS:
            return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED:
            return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS:
            return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC:
            return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE:
            return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT:
            return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG:
            return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE:
            return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY:
            return "EGL_BAD_DISPLAY";
        case EGL_BAD_SURFACE:
            return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH:
            return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER:
            return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP:
            return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW:
            return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST:
            return "EGL_CONTEXT_LOST";
        default:
            return "EGL_UNKNOWN_ERROR";
    }
}

std::string FormatEglError(const char* prefix) {
    std::ostringstream oss;
    const EGLint error = eglGetError();
    oss << prefix << " failed with " << EglErrorToString(error) << " (0x" << std::hex << error << ")";
    return oss.str();
}

}  // namespace

bool EglContext::Initialize(std::string* error) {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        if (error != nullptr) {
            *error = FormatEglError("eglGetDisplay");
        }
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(display_, &major, &minor) != EGL_TRUE) {
        if (error != nullptr) {
            *error = FormatEglError("eglInitialize");
        }
        return false;
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE};

    EGLint config_count = 0;
    if (eglChooseConfig(display_, config_attribs, &config_, 1, &config_count) != EGL_TRUE || config_count == 0) {
        if (error != nullptr) {
            *error = FormatEglError("eglChooseConfig");
        }
        return false;
    }

    const EGLint pbuffer_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    surface_ = eglCreatePbufferSurface(display_, config_, pbuffer_attribs);
    if (surface_ == EGL_NO_SURFACE) {
        if (error != nullptr) {
            *error = FormatEglError("eglCreatePbufferSurface");
        }
        return false;
    }

    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, context_attribs);
    if (context_ == EGL_NO_CONTEXT) {
        if (error != nullptr) {
            *error = FormatEglError("eglCreateContext");
        }
        return false;
    }

    if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) {
        if (error != nullptr) {
            *error = FormatEglError("eglMakeCurrent");
        }
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "EGL initialized. version=%d.%d", major, minor);
    return true;
}

void EglContext::Shutdown() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
        context_ = EGL_NO_CONTEXT;
    }
    if (surface_ != EGL_NO_SURFACE) {
        eglDestroySurface(display_, surface_);
        surface_ = EGL_NO_SURFACE;
    }
    if (display_ != EGL_NO_DISPLAY) {
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
    config_ = nullptr;
}

bool EglContext::IsValid() const noexcept {
    return display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT && config_ != nullptr;
}

}  // namespace vt::android
