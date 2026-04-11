cmake_minimum_required(VERSION 3.22.1)
project(videotest_android_native LANGUAGES C CXX)

set(VT_OPENXR_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/OpenXR-SDK-Source-main"
)

if(NOT EXISTS "${VT_OPENXR_SOURCE_DIR}/CMakeLists.txt")
    message(
        FATAL_ERROR
            "OpenXR-SDK-Source not found at ${VT_OPENXR_SOURCE_DIR}. Please ensure third_party/OpenXR-SDK-Source-main exists."
    )
endif()

set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_ALL_EXTENSIONS ON CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)

add_subdirectory(
    "${VT_OPENXR_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/openxr-sdk-build"
)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/cpp")
