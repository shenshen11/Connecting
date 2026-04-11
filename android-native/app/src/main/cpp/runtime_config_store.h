#pragma once

#include "xr_pose_runtime.h"

#include <android_native_app_glue.h>
#include <jni.h>

#include <string>

namespace vt::android {

struct RuntimeConfigResolveResult final {
    bool used_persisted = false;
    bool used_intent = false;
    std::string persisted_path;
};

RuntimeConfigResolveResult ResolveRuntimeConfig(android_app* app, JNIEnv* env, RuntimeConfig* config);
bool SaveLastSuccessfulRuntimeConfig(android_app* app, const RuntimeConfig& config, std::string* error);

}  // namespace vt::android
