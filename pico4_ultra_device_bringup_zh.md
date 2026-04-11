# PICO 4 Ultra 真机联调说明（中文更新版）

> 更新时间：2026-04-10

本文档用于说明当前项目在 PICO 4 Ultra 真机上的安装、启动和联调方式。内容已更新到当前实现状态，不再要求修改源码中的硬编码 IP。

---

## 1. 当前可安装 APK

APK 路径：

- `D:\videotest\android-native\app\build\outputs\apk\debug\app-debug.apk`

安装命令：

```powershell
adb install -r D:\videotest\android-native\app\build\outputs\apk\debug\app-debug.apk
```

---

## 2. 连接设备前提

请先确认：

- PICO 4 Ultra 已开启开发者模式
- ADB 连接正常
- PC 与头显处于可用网络环境中

检查命令：

```powershell
adb devices -l
adb shell ip addr show wlan0
ipconfig
```

建议每次换网络后都重新确认一次头显和 PC 的实时 IP。

---

## 3. 启动方式

### 3.1 直接启动

如果已经存在“上次成功配置”，可以直接启动：

```powershell
adb shell am force-stop com.videotest.nativeapp
adb shell am start -n com.videotest.nativeapp/android.app.NativeActivity
```

### 3.2 显式传入目标地址

如果当前网络发生变化，推荐显式传入运行时参数：

```powershell
adb shell am force-stop com.videotest.nativeapp
adb shell am start -n com.videotest.nativeapp/android.app.NativeActivity --es target_host 172.20.10.2 --ei target_port 25672 --ei video_port 25673 --ei encoded_video_port 25674
```

参数说明：

- `target_host`
  Windows PC 的当前 IP

- `target_port`
  头显上行 pose/control 默认目标端口

- `video_port`
  原始视频接收端口，当前主要作为历史/调试路径保留

- `encoded_video_port`
  H.264 编码视频接收端口，当前主用链路

---

## 4. 当前 Android 配置逻辑

头显端当前按以下优先级决定连接配置：

1. 内置默认值
2. 上次成功配置
3. `Intent` 启动参数覆盖

并且：

- 只有当第一次解码帧真正渲染成功后，才会把本次配置保存为“上次成功配置”
- 这样可以避免把一次失败尝试误记为可用地址

---

## 5. 建议同时打开的 Windows 程序

### 姿态接收调试

```powershell
D:\videotest\windows-native\build\Debug\pose_receiver.exe 25672
```

### 桌面视频下行

```powershell
D:\videotest\windows-native\build\Debug\desktop_capture_nvenc_sender.exe <headset_ip> 25674 10 4000000 0 1280 720 25672
```

### 姿态驱动原生场景下行

```powershell
D:\videotest\windows-native\build\Debug\pose_scene_nvenc_sender.exe <headset_ip> 25674 1280 720 15 4000000 25672
```

---

## 6. 真机联调时建议观察的日志

### Android 端

```powershell
adb logcat | findstr "videotest-native"
```

重点关注这些日志：

- `Resolved runtime config ...`
- `UDP sender opened for ...`
- `Session state changed -> ...`
- `Received codec config frame ...`
- `MediaCodec configured ...`
- `MediaCodec rendered first output frame ...`
- `Saved last successful runtime config ...`

### Windows 端

重点关注：

- pose 是否持续到达
- 是否发出了 codec config
- 是否连续输出编码帧
- `pose_scene_nvenc_sender` 是否进入 `pose=active`

---

## 7. 当前最重要的联调原则

- 每次换网络先查 IP，不要假设地址没变
- 先确认头显 App 真正在前台
- 先看协议链路日志，不要只看 `ping`
- `singleTask` Activity 在调试时最好先 `force-stop` 再 `am start`

---

## 8. 当前阶段的验收目标

真机接通后，优先验证这几个里程碑：

1. PICO 4 Ultra 原生 OpenXR App 能进入活动状态
2. Windows 端能稳定收到真实头显姿态
3. Windows 编码视频能被头显接收与解码
4. 姿态驱动的原生场景闭环能够连续运行
