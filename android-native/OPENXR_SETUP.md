# Android Native OpenXR 集成说明

当前 Android 工程已经不是最早的 stub 骨架，而是已经接入了 **OpenXR 第一版真实链路**。

## 1. 已接入的内容

当前实现包含：

1. `xrInitializeLoaderKHR`
2. `XR_KHR_android_create_instance`
3. `XR_KHR_opengl_es_enable`
4. EGL / OpenGL ES 3 最小上下文
5. `xrCreateSession`
6. `xrCreateReferenceSpace`
   - app space: `LOCAL`
   - head space: `VIEW`
7. 每帧：
   - `xrWaitFrame`
   - `xrBeginFrame`
   - `xrLocateSpace`
   - UDP pose 发送
   - `xrEndFrame`

## 2. 当前关键源码位置

### Native 入口
- `D:\videotest\android-native\app\src\main\cpp\openxr_pose_sender.cpp`

### OpenXR runtime 封装
- `D:\videotest\android-native\app\src\main\cpp\xr_pose_runtime.h`
- `D:\videotest\android-native\app\src\main\cpp\xr_pose_runtime.cpp`

### EGL 最小上下文
- `D:\videotest\android-native\app\src\main\cpp\egl_context.h`
- `D:\videotest\android-native\app\src\main\cpp\egl_context.cpp`

### UDP 发送器
- `D:\videotest\android-native\app\src\main\cpp\udp_pose_sender.h`
- `D:\videotest\android-native\app\src\main\cpp\udp_pose_sender.cpp`

## 3. OpenXR 依赖来源

当前工程通过本地 vendored 的官方 Khronos OpenXR-SDK-Source 源码构建 `openxr_loader`：

- `D:\videotest\third_party\OpenXR-SDK-Source-main`

Android 模块顶层 CMake：

- `D:\videotest\android-native\CMakeLists.txt`

会把 OpenXR loader 和本项目的 `xr_pose_sender` 一起编进 APK。

## 4. 当前未完成项

下面这些还没有做：

- 真机运行日志验证
- 坐标系实测校正
- pose 时间戳与 Windows 端对齐验证
- 视频解码显示
- MediaCodec 链路
- Unity 接入

## 5. 真机联调前要改的一个配置

目前头显端默认发送目标 IP 仍然写死在入口文件里：

- `D:\videotest\android-native\app\src\main\cpp\openxr_pose_sender.cpp`

你需要把：

- `192.168.1.100`

改成你的开发机局域网 IP，或者后续我们再把它改成可配置方式。
