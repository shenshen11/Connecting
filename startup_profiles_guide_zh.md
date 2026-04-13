# 启动脚本与 Profile 使用指南

本文档说明如何使用仓库中的启动脚本与 profile，减少手动输入长路径、长参数带来的启动成本。

## 目标

这套脚本主要解决三个问题：

- 把常用联调场景收敛成固定 profile；
- 自动同步头显运行时配置，降低换网后失败概率；
- 自动解析头显 Wi‑Fi IP，并用统一方式启动桌面 sender。

## 新增文件

- 通用启动器：`D:\videotest\tools\start_stream_profile.ps1`
- mixed demo 快捷脚本：`D:\videotest\tools\start_mixed_demo.ps1`
- Unity Editor 直接串流快捷脚本：`D:\videotest\tools\start_unity_editor_direct.ps1`
- Unity Editor 沉浸投影快捷脚本：`D:\videotest\tools\start_unity_editor_projection_mono.ps1`
- 桌面视频单独启动脚本：`D:\videotest\tools\start_desktop_only.ps1`
- profile 目录：`D:\videotest\tools\profiles\`

## profile 说明

### 1. `mixed_demo`

文件：`D:\videotest\tools\profiles\mixed_demo.json`

用途：

- VR 端看桌面视频；
- Unity Editor 只消费 pose；
- pose 与 control 分端口，避免本机冲突。

默认端口：

- `pose_target_port = 25672`
- `control_target_port = 25675`
- `encoded_video_port = 25674`

### 2. `unity_editor_direct`

文件：`D:\videotest\tools\profiles\unity_editor_direct.json`

用途：

- 不启动桌面 sender；
- 只同步头显运行时配置；
- 适合直接在 Unity Editor Play 中调试 Unity -> VR 串流。

### 3. `unity_editor_projection_mono`

文件：`D:\videotest\tools\profiles\unity_editor_projection_mono.json`

用途：

- 仍然使用 Unity Editor 直接串流；
- 头显侧把解码视频作为 `projection_mono` 提交给 OpenXR；
- 相比固定前方大屏，更适合作为沉浸显示的第一阶段验证。

### 4. `desktop_only`

文件：`D:\videotest\tools\profiles\desktop_only.json`

用途：

- 只看桌面下行视频；
- 不依赖 Unity；
- 适合单独检查视频链路是否正常。

## 推荐启动方式

建议先在仓库根目录 `D:\videotest` 打开 PowerShell，再执行脚本。这样命令最短。

### 查看可用 profile

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start_stream_profile.ps1 -ListProfiles
```

### 启动 mixed demo

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start_mixed_demo.ps1
```

执行后脚本会：

- 同步头显运行时配置；
- 从 adb 自动读取头显 `wlan0` IPv4；
- 启动 `desktop_capture_nvenc_sender.exe`；
- 把日志写到 `D:\videotest\tools\logs\`；
- 输出下一步提示。

### 启动 Unity Editor 直接串流联调

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start_unity_editor_direct.ps1
```

脚本只负责把头显侧配置对齐。执行完成后，再手动去 Unity Editor 点击 `Play`。

### 启动 Unity Editor 沉浸投影联调

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start_unity_editor_projection_mono.ps1
```

这个 profile 会把头显 `display_mode` 设置为 `projection_mono`，用于验证第一阶段的更沉浸显示方案。

### 启动桌面视频单链路

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start_desktop_only.ps1
```

## 常用覆盖参数

### 手动指定 PC IP

如果机器有多张网卡，自动解析出的 IP 不符合预期，可以手动指定：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start_mixed_demo.ps1 -PcIp 10.51.42.140
```

### 手动指定头显 IP

如果 adb 正常，但你不想依赖自动读取头显地址，可以直接传：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start_mixed_demo.ps1 -HeadsetIp 10.51.77.47
```

### 手动指定 adb 路径

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start_mixed_demo.ps1 -AdbPath D:\platform-tools-latest-windows\platform-tools\adb.exe
```

### 仅预览，不实际执行

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\start_mixed_demo.ps1 -DryRun
```

这个模式会打印最终命令、日志路径和解析结果，但不会真的重启头显或启动 sender。

### 手动指定显示模式

如果你想直接调用同步脚本而不走 profile，可以执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\sync_editor_runtime.ps1 -DisplayMode projection_mono
```

当前支持：

- `quad_mono`
- `projection_mono`
- `projection_stereo`

其中 `projection_stereo` 现在已经接通了 Android 侧双 decoder + 双 swapchain 的最小 true stereo 骨架；如果 Unity 端尚未切到 `StereoProjection`，运行时会暂时复用 primary view 继续可见，而不是直接黑屏。

## 日志与停止方式

桌面 sender 的日志写入：

- `D:\videotest\tools\logs\*_desktop_sender.out.log`
- `D:\videotest\tools\logs\*_desktop_sender.err.log`

如果需要停止已经启动的桌面 sender，可以执行：

```powershell
Get-Process desktop_capture_nvenc_sender -ErrorAction SilentlyContinue | Stop-Process -Force
```

## 典型使用流程

### mixed demo

1. 头显通过 USB 接到 PC，保证 `adb devices` 可见；
2. PC 与头显连到同一局域网；
3. 执行 `start_mixed_demo.ps1`；
4. 打开 Unity Editor 并点击 `Play`；
5. VR 端看到桌面视频，头显 pose 驱动 Unity 相机。

### Unity Editor 直接串流

1. 头显通过 USB 接到 PC，保证 `adb devices` 可见；
2. 如果 **PC 更换了 Wi‑Fi / 热点 / 局域网环境**，务必重新执行一次对应 profile 脚本，让头显运行时里的 `target_host` 改成当前 PC 的新 IP；
3. 执行 `start_unity_editor_direct.ps1`；
4. 回到 Unity Editor 点击 `Play`；
5. 观察 VR 端画面与 Unity Debug Overlay。

例如当前环境里，切网后如果头显仍保留旧的 `target_host=10.51.42.140`，而 PC 当前地址已经变成 `172.20.10.2`，那么即使 Unity 点击 `Play`，连接也不会建立，因为头显 pose / 控制 / 视频仍会发往旧地址。

## 扩展 profile 的方法

如果后续要新增场景，建议直接在 `D:\videotest\tools\profiles\` 下复制一个 JSON 修改：

- `syncRuntime`：头显运行时配置；
- `desktopSender`：桌面 sender 启动参数；
- `nextSteps`：脚本执行后打印给用户的提示。

这样后续就不需要再记忆完整命令行，只要记住 profile 名称或对应快捷脚本即可。
## 2026-04-13：Unity Editor profile 现在会同时同步头显与 Unity 运行时模式

从这一轮开始，`sync_editor_runtime.ps1` 不再只是：

- 写头显侧 `last_successful_runtime_config.txt`
- 重启头显 app

它还会额外写入 Unity 本地运行时配置文件：

- `C:\Users\Lenovo\AppData\LocalLow\DefaultCompany\pc-unity-app\VideoTestUnitySender\last_successful_endpoint.json`

这样做的目的，是把：

- 头显 `display_mode`
- Unity `captureViewMode`

联动起来，避免出现“头显已经切到 `projection_stereo`，但 Unity 还在 mono capture”的错配。

### 当前映射关系

- `quad_mono` -> Unity `captureViewMode=Mono`
- `projection_mono` -> Unity `captureViewMode=Mono`
- `projection_stereo` -> Unity `captureViewMode=StereoProjection`

### 新增 profile

- `unity_editor_projection_mono`
  - 头显：`projection_mono`
  - Unity：`Mono`
- `unity_editor_projection_stereo`
  - 头显：`projection_stereo`
  - Unity：`StereoProjection`

### 推荐用法

#### 验证 mono projection

```powershell
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\start_unity_editor_projection_mono.ps1
```

#### 验证 stereo projection

```powershell
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\start_unity_editor_projection_stereo.ps1
```

运行后再回到 Unity Editor 点击 `Play`。
如果中途更换了 Wi‑Fi / 热点 / 局域网环境，仍然需要重新执行对应 profile，一次性同时更新：

- 头显 `target_host`
- Unity 本地保存的目标地址
- Unity 当前 `captureViewMode`
