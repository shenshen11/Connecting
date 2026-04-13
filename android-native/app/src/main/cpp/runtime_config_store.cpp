#include "runtime_config_store.h"

#include <android/log.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace vt::android {
namespace {

constexpr const char* kLogTag = "videotest-native";
constexpr const char* kConfigFilename = "last_successful_runtime_config.txt";

std::string Trim(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool TryParsePort(const std::string& text, std::uint16_t* out_port) {
    if (out_port == nullptr) {
        return false;
    }

    try {
        const unsigned long parsed = std::stoul(text);
        if (parsed == 0 || parsed > 65535) {
            return false;
        }
        *out_port = static_cast<std::uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool TryParseDisplayMode(const std::string& text, DecodedVideoDisplayMode* out_mode) {
    if (out_mode == nullptr) {
        return false;
    }

    const std::string normalized = Trim(text);
    if (normalized == "quad_mono") {
        *out_mode = DecodedVideoDisplayMode::QuadMono;
        return true;
    }
    if (normalized == "projection_mono") {
        *out_mode = DecodedVideoDisplayMode::ProjectionMono;
        return true;
    }
    if (normalized == "projection_stereo") {
        *out_mode = DecodedVideoDisplayMode::ProjectionStereo;
        return true;
    }
    return false;
}

std::string MakePersistedConfigPath(android_app* app) {
    if (app == nullptr || app->activity == nullptr || app->activity->internalDataPath == nullptr) {
        return {};
    }

    std::string path = app->activity->internalDataPath;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path += kConfigFilename;
    return path;
}

bool LoadPersistedRuntimeConfig(android_app* app, RuntimeConfig* config, std::string* out_path) {
    if (config == nullptr) {
        return false;
    }

    const std::string path = MakePersistedConfigPath(app);
    if (out_path != nullptr) {
        *out_path = path;
    }
    if (path.empty()) {
        return false;
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        return false;
    }

    RuntimeConfig loaded = *config;
    bool used_any = false;
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        const std::size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = Trim(trimmed.substr(0, separator));
        const std::string value = Trim(trimmed.substr(separator + 1));
        if (key == "target_host") {
            if (!value.empty()) {
                loaded.target_host = value;
                used_any = true;
            }
            continue;
        }

        if (key == "display_mode") {
            DecodedVideoDisplayMode display_mode = loaded.display_mode;
            if (TryParseDisplayMode(value, &display_mode)) {
                loaded.display_mode = display_mode;
                used_any = true;
            }
            continue;
        }

        std::uint16_t port = 0;
        if (!TryParsePort(value, &port)) {
            continue;
        }

        if (key == "target_port") {
            loaded.pose_target_port = port;
            loaded.control_target_port = port;
            used_any = true;
        } else if (key == "pose_target_port") {
            loaded.pose_target_port = port;
            used_any = true;
        } else if (key == "control_target_port") {
            loaded.control_target_port = port;
            used_any = true;
        } else if (key == "video_port") {
            loaded.video_port = port;
            used_any = true;
        } else if (key == "encoded_video_port") {
            loaded.encoded_video_port = port;
            used_any = true;
        }
    }

    if (used_any) {
        *config = loaded;
    }
    return used_any;
}

std::string GetStringExtra(JNIEnv* env, jobject intent, const char* key) {
    if (env == nullptr || intent == nullptr || key == nullptr) {
        return {};
    }

    jclass intent_class = env->GetObjectClass(intent);
    if (intent_class == nullptr) {
        return {};
    }

    jmethodID has_extra = env->GetMethodID(intent_class, "hasExtra", "(Ljava/lang/String;)Z");
    jmethodID get_string_extra =
        env->GetMethodID(intent_class, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
    if (has_extra == nullptr || get_string_extra == nullptr) {
        env->DeleteLocalRef(intent_class);
        return {};
    }

    jstring key_string = env->NewStringUTF(key);
    if (key_string == nullptr) {
        env->DeleteLocalRef(intent_class);
        return {};
    }

    const jboolean present = env->CallBooleanMethod(intent, has_extra, key_string);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(key_string);
        env->DeleteLocalRef(intent_class);
        return {};
    }
    if (!present) {
        env->DeleteLocalRef(key_string);
        env->DeleteLocalRef(intent_class);
        return {};
    }

    auto* value_string = static_cast<jstring>(env->CallObjectMethod(intent, get_string_extra, key_string));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(key_string);
        env->DeleteLocalRef(intent_class);
        return {};
    }

    std::string result;
    if (value_string != nullptr) {
        const char* chars = env->GetStringUTFChars(value_string, nullptr);
        if (chars != nullptr) {
            result = chars;
            env->ReleaseStringUTFChars(value_string, chars);
        }
        env->DeleteLocalRef(value_string);
    }

    env->DeleteLocalRef(key_string);
    env->DeleteLocalRef(intent_class);
    return result;
}

bool GetIntExtra(JNIEnv* env, jobject intent, const char* key, std::uint16_t* out_value) {
    if (env == nullptr || intent == nullptr || key == nullptr || out_value == nullptr) {
        return false;
    }

    jclass intent_class = env->GetObjectClass(intent);
    if (intent_class == nullptr) {
        return false;
    }

    jmethodID has_extra = env->GetMethodID(intent_class, "hasExtra", "(Ljava/lang/String;)Z");
    jmethodID get_int_extra = env->GetMethodID(intent_class, "getIntExtra", "(Ljava/lang/String;I)I");
    if (has_extra == nullptr || get_int_extra == nullptr) {
        env->DeleteLocalRef(intent_class);
        return false;
    }

    jstring key_string = env->NewStringUTF(key);
    if (key_string == nullptr) {
        env->DeleteLocalRef(intent_class);
        return false;
    }

    const jboolean present = env->CallBooleanMethod(intent, has_extra, key_string);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(key_string);
        env->DeleteLocalRef(intent_class);
        return false;
    }
    if (!present) {
        env->DeleteLocalRef(key_string);
        env->DeleteLocalRef(intent_class);
        return false;
    }

    const jint value = env->CallIntMethod(intent, get_int_extra, key_string, static_cast<jint>(*out_value));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(key_string);
        env->DeleteLocalRef(intent_class);
        return false;
    }

    env->DeleteLocalRef(key_string);
    env->DeleteLocalRef(intent_class);

    if (value <= 0 || value > 65535) {
        return false;
    }

    *out_value = static_cast<std::uint16_t>(value);
    return true;
}

bool ApplyIntentRuntimeConfig(android_app* app, JNIEnv* env, RuntimeConfig* config) {
    if (app == nullptr || app->activity == nullptr || env == nullptr || config == nullptr) {
        return false;
    }

    jobject activity = app->activity->clazz;
    if (activity == nullptr) {
        return false;
    }

    jclass activity_class = env->GetObjectClass(activity);
    if (activity_class == nullptr) {
        return false;
    }

    jmethodID get_intent = env->GetMethodID(activity_class, "getIntent", "()Landroid/content/Intent;");
    if (get_intent == nullptr) {
        env->DeleteLocalRef(activity_class);
        return false;
    }

    jobject intent = env->CallObjectMethod(activity, get_intent);
    env->DeleteLocalRef(activity_class);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    if (intent == nullptr) {
        return false;
    }

    bool used_any = false;

    const std::string target_host = GetStringExtra(env, intent, "target_host");
    if (!target_host.empty()) {
        config->target_host = target_host;
        used_any = true;
    }

    const std::string display_mode_text = GetStringExtra(env, intent, "display_mode");
    if (!display_mode_text.empty()) {
        DecodedVideoDisplayMode display_mode = config->display_mode;
        if (TryParseDisplayMode(display_mode_text, &display_mode)) {
            config->display_mode = display_mode;
            used_any = true;
        }
    }

    std::uint16_t port_value = config->pose_target_port;
    if (GetIntExtra(env, intent, "target_port", &port_value)) {
        config->pose_target_port = port_value;
        config->control_target_port = port_value;
        used_any = true;
    }

    port_value = config->pose_target_port;
    if (GetIntExtra(env, intent, "pose_target_port", &port_value)) {
        config->pose_target_port = port_value;
        used_any = true;
    }

    port_value = config->control_target_port;
    if (GetIntExtra(env, intent, "control_target_port", &port_value)) {
        config->control_target_port = port_value;
        used_any = true;
    }

    port_value = config->video_port;
    if (GetIntExtra(env, intent, "video_port", &port_value)) {
        config->video_port = port_value;
        used_any = true;
    }

    port_value = config->encoded_video_port;
    if (GetIntExtra(env, intent, "encoded_video_port", &port_value)) {
        config->encoded_video_port = port_value;
        used_any = true;
    }

    env->DeleteLocalRef(intent);
    return used_any;
}

}  // namespace

RuntimeConfigResolveResult ResolveRuntimeConfig(android_app* app, JNIEnv* env, RuntimeConfig* config) {
    RuntimeConfigResolveResult result{};
    if (config == nullptr) {
        return result;
    }

    result.used_persisted = LoadPersistedRuntimeConfig(app, config, &result.persisted_path);
    result.used_intent = ApplyIntentRuntimeConfig(app, env, config);

    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "Resolved runtime config target=%s pose_target_port=%u control_target_port=%u video_port=%u encoded_video_port=%u display_mode=%s source=%s%s%s",
                        config->target_host.c_str(),
                        static_cast<unsigned>(config->pose_target_port),
                        static_cast<unsigned>(config->control_target_port),
                        static_cast<unsigned>(config->video_port),
                        static_cast<unsigned>(config->encoded_video_port),
                        DecodedVideoDisplayModeName(config->display_mode),
                        "defaults",
                        result.used_persisted ? "+persisted" : "",
                        result.used_intent ? "+intent" : "");
    return result;
}

bool SaveLastSuccessfulRuntimeConfig(android_app* app, const RuntimeConfig& config, std::string* error) {
    const std::string path = MakePersistedConfigPath(app);
    if (path.empty()) {
        if (error != nullptr) {
            *error = "internalDataPath is unavailable";
        }
        return false;
    }

    const std::string temp_path = path + ".tmp";
    std::ofstream output(temp_path, std::ios::trunc);
    if (!output.is_open()) {
        if (error != nullptr) {
            *error = "failed to open config temp file for write";
        }
        return false;
    }

    output << "# Saved after first successful decoded frame\n";
    output << "target_host=" << config.target_host << "\n";
    if (config.pose_target_port == config.control_target_port) {
        output << "target_port=" << config.pose_target_port << "\n";
    }
    output << "pose_target_port=" << config.pose_target_port << "\n";
    output << "control_target_port=" << config.control_target_port << "\n";
    output << "video_port=" << config.video_port << "\n";
    output << "encoded_video_port=" << config.encoded_video_port << "\n";
    output << "display_mode=" << DecodedVideoDisplayModeName(config.display_mode) << "\n";
    output.close();

    if (!output) {
        if (error != nullptr) {
            *error = "failed to flush config temp file";
        }
        std::remove(temp_path.c_str());
        return false;
    }

    std::remove(path.c_str());
    if (std::rename(temp_path.c_str(), path.c_str()) != 0) {
        if (error != nullptr) {
            *error = "failed to rename config temp file";
        }
        std::remove(temp_path.c_str());
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO,
                        kLogTag,
                        "Saved last successful runtime config target=%s pose_target_port=%u control_target_port=%u display_mode=%s path=%s",
                        config.target_host.c_str(),
                        static_cast<unsigned>(config.pose_target_port),
                        static_cast<unsigned>(config.control_target_port),
                        DecodedVideoDisplayModeName(config.display_mode),
                        path.c_str());
    return true;
}

}  // namespace vt::android
