#pragma once

#include <EGL/egl.h>

#include <string>

namespace vt::android {

class EglContext final {
public:
    bool Initialize(std::string* error);
    void Shutdown();

    bool IsValid() const noexcept;

    EGLDisplay display() const noexcept { return display_; }
    EGLConfig config() const noexcept { return config_; }
    EGLContext context() const noexcept { return context_; }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
};

}  // namespace vt::android
