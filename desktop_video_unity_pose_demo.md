# Desktop Video + Unity Pose Demo

## 目标

这个 demo 的目标是：

- VR 端看到的是 `desktop_capture_nvenc_sender.exe` 推送的桌面视频
- Unity Editor 只消费头显 pose，用来实时更新 Unity 相机
- 视频下行和 pose 上行不再强绑定到同一个 UDP 上行端口

## 端口规划

推荐使用下面这组端口：

- `pose_target_port = 25672`：头显 pose 发给 Unity
- `control_target_port = 25675`：头显 decoder control 发给桌面 sender
- `encoded_video_port = 25674`：桌面 sender 发给头显的视频码流

这样可以避免：

- Unity 插件监听 `25672`
- 桌面 sender 也监听 `25672`

两者在同一台 PC 上发生端口冲突。

## 兼容性

头显侧现在同时支持两种配置方式：

1. 旧方式：只传 `target_port`
   - pose / control 仍然共用一个端口
2. 新方式：分别传 `pose_target_port` 和 `control_target_port`
   - 用于这个 mixed demo

如果只提供旧参数，系统会自动把它同时作为 pose/control 端口使用。

## 推荐启动方式

### 1. 同步头显运行时配置

```powershell
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\sync_editor_runtime.ps1 -PoseTargetPort 25672 -ControlTargetPort 25675
```

这个脚本会：

- 自动解析当前 PC IPv4
- 重置 Unity 保存的旧 endpoint
- 把头显持久化运行时配置写成分端口模式
- 重启头显 app 并带上新的 intent extra

### 2. 启动桌面视频 sender

```powershell
D:\videotest\windows-native\build\Debug\desktop_capture_nvenc_sender.exe <headset_ip> 25674 10 4000000 0 1280 720 25675
```

最后一个参数 `25675` 是桌面 sender 的 `control_port`，必须和头显侧的 `control_target_port` 对齐。

### 3. 在 Unity Editor 中 Play

保持 Unity 里的 `posePort = 25672`，然后点击 `Play`。

此时预期行为是：

- VR 端显示桌面视频
- 头显 pose 持续发到 Unity
- Unity 相机跟随头显 pose 更新

## 改动点

这次改造的关键点是：

- `android-native/app/src/main/cpp/xr_pose_runtime.h`
  - `RuntimeConfig` 拆为 `pose_target_port` 和 `control_target_port`
- `android-native/app/src/main/cpp/xr_pose_runtime.cpp`
  - 头显侧分别打开 pose sender 和 control sender
- `android-native/app/src/main/cpp/runtime_config_store.cpp`
  - 持久化配置和 intent extra 同时支持旧/新两套字段
- `tools/sync_editor_runtime.ps1`
  - 新增 `-PoseTargetPort` / `-ControlTargetPort`
  - 继续兼容旧的 `-TargetPort`
