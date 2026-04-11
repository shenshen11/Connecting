cmake_minimum_required(VERSION 3.20)
project(videotest_windows_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VT_NVIDIA_SDK_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nvidia-video-sdk-samples/video-sdk-samples-master/Samples"
)

set(VT_UNITY_PLUGIN_API_DIR "")
if(DEFINED ENV{UNITY_PLUGIN_API_DIR} AND EXISTS "$ENV{UNITY_PLUGIN_API_DIR}/IUnityGraphics.h")
    set(VT_UNITY_PLUGIN_API_DIR "$ENV{UNITY_PLUGIN_API_DIR}")
endif()

if(NOT VT_UNITY_PLUGIN_API_DIR)
    file(GLOB VT_UNITY_PLUGIN_API_CANDIDATES "C:/Program Files/Unity/Hub/Editor/*/Editor/Data/PluginAPI")
    list(SORT VT_UNITY_PLUGIN_API_CANDIDATES ORDER DESCENDING)
    foreach(candidate ${VT_UNITY_PLUGIN_API_CANDIDATES})
        if(EXISTS "${candidate}/IUnityGraphics.h")
            set(VT_UNITY_PLUGIN_API_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()

add_executable(pose_receiver
    src/main.cpp
    src/udp_pose_receiver.cpp
)

target_include_directories(pose_receiver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared-protocol
)

target_link_libraries(pose_receiver PRIVATE ws2_32)

add_executable(mock_pose_sender
    src/mock_pose_sender.cpp
)

target_include_directories(mock_pose_sender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared-protocol
)

target_link_libraries(mock_pose_sender PRIVATE ws2_32)

add_executable(mock_video_sender
    src/mock_video_sender.cpp
)

target_include_directories(mock_video_sender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared-protocol
)

target_link_libraries(mock_video_sender PRIVATE ws2_32)

add_executable(nvenc_video_sender
    src/nvenc_video_sender.cpp
    ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder/NvEncoder.cpp
    ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder/NvEncoderD3D11.cpp
)

target_include_directories(nvenc_video_sender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared-protocol
    ${VT_NVIDIA_SDK_DIR}
    ${VT_NVIDIA_SDK_DIR}/NvCodec
    ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder
)

target_link_libraries(nvenc_video_sender PRIVATE
    ws2_32
    d3d11
    dxgi
)

add_executable(desktop_capture_nvenc_sender
    src/desktop_capture_nvenc_sender.cpp
    src/video_sender_core.cpp
    ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder/NvEncoder.cpp
    ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder/NvEncoderD3D11.cpp
)

target_include_directories(desktop_capture_nvenc_sender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared-protocol
    ${VT_NVIDIA_SDK_DIR}
    ${VT_NVIDIA_SDK_DIR}/NvCodec
    ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder
)

target_link_libraries(desktop_capture_nvenc_sender PRIVATE
    ws2_32
    d3d11
    d3dcompiler
    dxgi
)

add_executable(pose_scene_nvenc_sender
    src/pose_scene_nvenc_sender.cpp
    src/video_sender_core.cpp
    ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder/NvEncoder.cpp
    ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder/NvEncoderD3D11.cpp
)

target_include_directories(pose_scene_nvenc_sender PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared-protocol
    ${VT_NVIDIA_SDK_DIR}
    ${VT_NVIDIA_SDK_DIR}/NvCodec
    ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder
)

target_link_libraries(pose_scene_nvenc_sender PRIVATE
    ws2_32
    d3d11
    d3dcompiler
)

if(VT_UNITY_PLUGIN_API_DIR)
    add_library(unity_sender_plugin SHARED
        src/unity_sender_plugin.cpp
        src/video_sender_core.cpp
        ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder/NvEncoder.cpp
        ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder/NvEncoderD3D11.cpp
    )

    target_include_directories(unity_sender_plugin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared-protocol
        ${VT_NVIDIA_SDK_DIR}
        ${VT_NVIDIA_SDK_DIR}/NvCodec
        ${VT_NVIDIA_SDK_DIR}/NvCodec/NvEncoder
        ${VT_UNITY_PLUGIN_API_DIR}
    )

    target_link_libraries(unity_sender_plugin PRIVATE
        ws2_32
        d3d11
        dxgi
    )

    target_compile_definitions(unity_sender_plugin PRIVATE
        NOMINMAX
    )
else()
    message(WARNING "Unity PluginAPI headers not found; unity_sender_plugin target will not be generated.")
endif()
