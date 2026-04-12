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

### 3. `desktop_only`

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
2. 执行 `start_unity_editor_direct.ps1`；
3. 回到 Unity Editor 点击 `Play`；
4. 观察 VR 端画面与 Unity Debug Overlay。

## 扩展 profile 的方法

如果后续要新增场景，建议直接在 `D:\videotest\tools\profiles\` 下复制一个 JSON 修改：

- `syncRuntime`：头显运行时配置；
- `desktopSender`：桌面 sender 启动参数；
- `nextSteps`：脚本执行后打印给用户的提示。

这样后续就不需要再记忆完整命令行，只要记住 profile 名称或对应快捷脚本即可。
