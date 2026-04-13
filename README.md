# videotest

当前仓库用于开发“类 PICO VR 互联”的原生原型，目标是在 PC 端渲染内容、编码传输到头显，并将头显 pose 实时回传到 PC 端驱动相机或场景。

## 当前进展

- 已具备视频下行链路。
- 已具备 pose 上行链路。
- Windows 桌面抓取与 D3D11 直接渲染样例已跑通。
- Unity 已接入为渲染内容源。
- **当前推荐开发路径**：优先在 `Unity Editor Play` 模式下完成双向链路调试，再回到 `Unity.exe` 打包验证。
- 已新增可切换的沉浸式显示模式：`quad_mono`、`projection_mono`、`projection_stereo`。
- `projection_mono` 已验证可见，`projection_stereo` 已接通 Unity 双眼纹理、Windows 双 sender、Android 双 decoder 与 OpenXR `XrCompositionLayerProjection` 骨架。
- 目前已验证：
  - Unity Editor 渲染画面可发送到 VR 端。
  - VR 端 pose 可实时回传到 Unity。
  - Unity 相机可根据回传 pose 更新。
  - VR 端画面上下反转问题已修复。

## 当前技术路线

- 头显侧：`Android Native + OpenXR`
  - 接收视频流
  - 解码并显示
  - 回传头显 pose
- PC 侧：`Windows Native + D3D11 + NVENC + Unity`
  - Unity 负责内容渲染
  - 原生插件负责纹理采集、编码和发送
  - 原生接收器负责接收 pose 并回传给 Unity
- 显示层路线：`XrCompositionLayerQuad` -> `GL-backed XrCompositionLayerProjection` -> true stereo projection

## 更沉浸的视频显示路线

- `quad_mono`
  - 旧路径：单路视频贴到 OpenXR quad layer，表现为“前方大屏”。
- `projection_mono`
  - 过渡路径：仍是单路解码，但改为通过 `XrCompositionLayerProjection` 提交，先验证 projection 显示链路。
- `projection_stereo`
  - 当前主线：Unity 输出左右眼两张 `RenderTexture`，Windows 原生插件分别编码发送，Android 按 `view_id` 路由到左右 decoder，再把左右眼 swapchain 提交给 OpenXR compositor。

当前推荐的联调入口：

```powershell
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\start_unity_editor_projection_mono.ps1
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\start_unity_editor_projection_stereo.ps1
```

如果只是同步当前网络环境与头显运行时配置，也可以继续使用：

```powershell
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\sync_editor_runtime.ps1
```

## Unity Editor 联调建议流程

当 PC 和头显切换了 Wi‑Fi、热点或网段后，建议先同步一次运行时地址，再点击 `Play`。

### 1. 连接同一局域网

- PC 与头显连接到同一网络。
- 确保头显已通过 adb 可见。

### 2. 同步运行时配置

执行：

```powershell
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\sync_editor_runtime.ps1
```

脚本会自动完成：

- 清理 Unity 保存的旧成功端点；
- 解析当前 PC 可用 IPv4；
- 更新头显持久化运行时配置；
- 使用正确参数重启头显 app。

如果需要手动指定 PC 地址，可以执行：

```powershell
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\sync_editor_runtime.ps1 -PcIp 10.51.42.140
```

### 3. 进入 Unity Play

- 打开 Unity 项目：`pc-unity-app/`
- 打开用于联调的场景并点击 `Play`
- 确认头显端应用在前台

### 4. 观察联调信号

如果链路正常，通常可以看到：

- Unity 日志中的 `pose_packets` 持续增长；
- Unity 状态从 `pose=default` 变为 `pose=active`；
- 头显端持续出现 `Pose sent`；
- 头显端持续出现 `Rendered output frame`。

### 5. 常见问题

#### VR 端画面上下反转

如果 VR 端能看到画面，但方向上下颠倒，优先检查头显侧 OpenXR 组合层是否正确声明了图像布局。

当前仓库中的修复方式是：

- 在 `android-native/app/src/main/cpp/xr_pose_runtime.cpp` 中启用 `XR_FB_composition_layer_image_layout`
- 给视频 `XrCompositionLayerQuad` 挂接 `XrCompositionLayerImageLayoutFB`
- 设置 `XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB`

这样可以告诉设备运行时：当前 swapchain 图像需要按垂直翻转解释，而不是在 Unity 或编码路径里做额外的像素翻转。

## 目录说明

- `architecture_direction.md`：总体架构方向说明
- `phase1_native_plan.md`：第一阶段原生开发计划
- `current_architecture_guide.md`：当前架构与联调说明
- `pico4_ultra_device_bringup.md`：PICO 4 Ultra 真机联调说明
- `shared-protocol/`：共享协议定义
- `windows-native/`：Windows 原生模块
- `android-native/`：Android Native 模块
- `pc-unity-app/`：Unity PC 侧工程
- `unity-integration/`：Unity 集成相关资料
- `tools/`：辅助脚本与工具

## 排障资料

- `unity_editor_playmode_debug_recap_zh.md`：本次 Unity Editor Play 双向链路问题的完整排查复盘
- `pico4_ultra_device_bringup_zh.md`：设备与真机联调说明
- `development_journal_zh.md`：开发过程记录
- `immersive_video_display_plan_zh.md`：更沉浸的视频显示方案阶段计划
- `immersive_projection_pathfinding_log_zh.md`：从 quad 过渡到 projection / stereo projection 的探索与问题留痕
- `startup_profiles_guide_zh.md`：当前启动 profile、参数转发与推荐用法

## 当前状态

当前仓库已不再只是第一阶段代码骨架，而是进入 **Unity Editor 双向链路稳定化与开发体验优化** 阶段。近期重点包括：

- 提升 Editor Play 模式稳定性；
- 降低换网后旧缓存地址导致的联调失败概率；
- 持续完善 Unity -> VR 视频链路与 VR -> Unity pose 链路的诊断能力。
- 在 `projection_stereo` 路线上继续推进双眼同步、性能与网络鲁棒性优化。
