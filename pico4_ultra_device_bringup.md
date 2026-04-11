# PICO 4 Ultra 真机联调准备说明

目前项目状态已经更新：

- Windows 原生 pose receiver 已可用
- Windows mock sender 已可自测
- Android Native + OpenXR 第一版已成功构建 APK

## 1. 当前可安装 APK

APK 路径：

- `D:\videotest\android-native\app\build\outputs\apk\debug\app-debug.apk`

## 2. 当前需要做的真机步骤

### 2.1 连接设备

先确认 PICO 4 Ultra 已开启开发者模式，并通过 USB 或无线 adb 连接。

检查命令：

```powershell
D:\platform-tools-latest-windows\platform-tools\adb.exe devices -l
```

当前在本环境里执行时，**还没有看到已连接设备**，所以还不能直接安装。

### 2.2 安装 APK

当 `adb devices` 能看到设备后，安装命令：

```powershell
D:\platform-tools-latest-windows\platform-tools\adb.exe install -r D:\videotest\android-native\app\build\outputs\apk\debug\app-debug.apk
```

### 2.3 查看日志

```powershell
D:\platform-tools-latest-windows\platform-tools\adb.exe logcat | Select-String "videotest-native"
```

## 3. 联调时建议同时打开的 Windows 程序

### 姿态接收端

```powershell
D:\videotest\windows-native\build\Debug\pose_receiver.exe 25672
```

这样可以直接看到头显端发送回来的 pose 包。

## 4. 联调前必须确认的一件事

Android 发送端当前默认目标 IP 仍然是：

- `192.168.1.100`

代码位置：

- `D:\videotest\android-native\app\src\main\cpp\openxr_pose_sender.cpp`

真机联调前，请把它改成你当前开发机在局域网内的实际 IP。

## 5. 下一步目标

真机接通后，我们要验证的第一个里程碑是：

> PICO 4 Ultra 上运行原生 OpenXR App，Windows 端 `pose_receiver.exe` 能稳定收到真实头显姿态。
