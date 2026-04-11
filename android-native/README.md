# android-native

该目录现在已经从“纯骨架/stub”推进到**可构建的 Android Native + OpenXR 第一版**。

## 当前状态

目前这条链路已经具备：

- NativeActivity 入口
- OpenXR Android loader 初始化（`xrInitializeLoaderKHR`）
- OpenXR instance / system / session 创建
- OpenGL ES/EGL 最小上下文初始化
- `XR_REFERENCE_SPACE_TYPE_LOCAL` 作为应用空间
- `XR_REFERENCE_SPACE_TYPE_VIEW` 作为头部空间
- 每帧 `xrWaitFrame` / `xrBeginFrame` / `xrLocateSpace` 采集 HMD pose
- 通过 UDP 将 pose 发回 Windows
- OpenXR loader 已随 APK 一起构建打包

## 当前构建结果

已成功在本机执行：

```powershell
D:\videotest\android-native\gradlew.bat assembleDebug --console plain
```

生成的 APK 位于：

- `D:\videotest\android-native\app\build\outputs\apk\debug\app-debug.apk`

## 当前限制

1. 发送目标 IP 仍然是代码中的占位值：
   - `192.168.1.100`
2. 还没有完成真机安装与运行验证
3. 还没有开始视频接收/解码链路
4. 还没有接入 Unity

## 下一步（设备联调）

当 PICO 4 Ultra 通过 adb 正常连接后，下一步就是：

1. `adb devices` 确认设备在线
2. 安装 APK
3. 通过 `adb logcat` 看 OpenXR 初始化和 pose 发送日志
4. 在 Windows 端启动 `pose_receiver.exe` 看是否收到真机姿态

更多设备联调说明见：

- `D:\videotest\pico4_ultra_device_bringup.md`
- `D:\videotest\android-native\OPENXR_SETUP.md`
