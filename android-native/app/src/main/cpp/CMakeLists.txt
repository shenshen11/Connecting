cmake_minimum_required(VERSION 3.22.1)

find_path(
    ANDROID_NATIVE_APP_GLUE android_native_app_glue.h
    PATHS
        "${ANDROID_NDK}/sources/android/native_app_glue"
        "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue"
)

if(NOT ANDROID_NATIVE_APP_GLUE)
    message(FATAL_ERROR "android_native_app_glue.h not found in the active NDK.")
endif()

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)
find_library(media-lib mediandk)

add_library(
    xr_pose_sender
    SHARED
    openxr_pose_sender.cpp
    xr_pose_runtime.cpp
    runtime_config_store.cpp
    egl_context.cpp
    udp_pose_sender.cpp
    udp_video_receiver.cpp
    udp_encoded_video_receiver.cpp
    amedia_h264_decoder.cpp
    $<TARGET_OBJECTS:android_native_app_glue>
)

target_include_directories(
    xr_pose_sender
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${ANDROID_NATIVE_APP_GLUE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../shared-protocol
)

target_compile_definitions(
    xr_pose_sender
    PRIVATE
        XR_USE_PLATFORM_ANDROID
        XR_USE_PLATFORM_EGL
        XR_USE_GRAPHICS_API_OPENGL_ES
)

target_link_libraries(
    xr_pose_sender
    PRIVATE
        OpenXR::openxr_loader
        ${log-lib}
        ${android-lib}
        ${egl-lib}
        ${gles-lib}
        ${media-lib}
)
