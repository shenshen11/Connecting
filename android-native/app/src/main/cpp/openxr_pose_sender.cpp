#include "xr_pose_runtime.h"
#include "runtime_config_store.h"

#include <android/log.h>
#include <android_native_app_glue.h>

#include <chrono>
#include <thread>

namespace {

constexpr const char* kLogTag = "videotest-native";

struct AppState final {
    bool resumed = false;
};

void HandleAppCommand(android_app* app, int32_t cmd) {
    auto* app_state = reinterpret_cast<AppState*>(app->userData);
    if (app_state == nullptr) {
        return;
    }

    switch (cmd) {
        case APP_CMD_RESUME:
            app_state->resumed = true;
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "APP_CMD_RESUME");
            break;
        case APP_CMD_PAUSE:
            app_state->resumed = false;
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "APP_CMD_PAUSE");
            break;
        case APP_CMD_INIT_WINDOW:
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "APP_CMD_INIT_WINDOW");
            break;
        case APP_CMD_TERM_WINDOW:
            __android_log_print(ANDROID_LOG_INFO, kLogTag, "APP_CMD_TERM_WINDOW");
            break;
        default:
            break;
    }
}

}  // namespace

void android_main(android_app* app) {
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    AppState app_state{};
    app->userData = &app_state;
    app->onAppCmd = HandleAppCommand;

    vt::android::RuntimeConfig config{};
    vt::android::ResolveRuntimeConfig(app, env, &config);

    vt::android::XrPoseRuntime runtime;
    std::string error;
    if (!runtime.Initialize(app, config, &error)) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Runtime initialization failed: %s", error.c_str());
        app->activity->vm->DetachCurrentThread();
        return;
    }

    bool request_restart = false;
    bool exit_render_loop = false;

    while (app->destroyRequested == 0) {
        for (int poll_iteration = 0; poll_iteration < 8; ++poll_iteration) {
            int events = 0;
            android_poll_source* source = nullptr;

            const int timeout_milliseconds = (poll_iteration == 0 &&
                                              !app_state.resumed &&
                                              !runtime.IsSessionRunning() &&
                                              app->destroyRequested == 0)
                                                 ? 10
                                                 : 0;
            const int poll_result =
                ALooper_pollOnce(timeout_milliseconds, nullptr, &events, reinterpret_cast<void**>(&source));
            if (poll_result < 0) {
                break;
            }

            if (source != nullptr) {
                source->process(app, source);
            }
        }

        runtime.PollEvents(&exit_render_loop, &request_restart);
        if (exit_render_loop) {
            ANativeActivity_finish(app->activity);
            continue;
        }

        runtime.TickBackgroundWork();

        if (!runtime.IsSessionRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        runtime.RunFrame();
    }

    runtime.Shutdown();
    app->activity->vm->DetachCurrentThread();
}
