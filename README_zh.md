# videotest 中文说明

当前仓库用于开发一个“类 PICO VR 互联”的原生系统原型，目标是在 **Windows PC** 与 **PICO 4 Ultra** 之间打通一条完整的低层链路：

- 头显端采集头部姿态并上行
- Windows 端根据姿态更新内容
- Windows 端原生编码视频并下行
- 头显端原生解码并在 OpenXR 中显示

当前阶段的工程策略是：

- **Unity 后接入**
- **先做原生链路**
- **先验证闭环，再做产品化**

---

## 当前技术路线

- Windows 端：
  - 原生 C++
  - D3D11
  - NVENC
  - UDP 自定义协议

- 头显端：
  - Android Native
  - OpenXR
  - MediaCodec
  - OpenGL ES

- 中间层：
  - `shared-protocol/` 中统一定义 pose、video、control 的包结构

---

## 当前目录说明

- [architecture_direction.md](D:/videotest/architecture_direction.md)
  总体架构方向说明，解释为什么先做原生、为什么暂缓 Unity。

- [phase1_native_plan_zh.md](D:/videotest/phase1_native_plan_zh.md)
  第一阶段的原生开发计划，强调开发顺序与阶段交付物。

- [current_architecture_guide_zh.md](D:/videotest/current_architecture_guide_zh.md)
  当前系统架构的完整中文说明，是理解现状最重要的文档之一。

- [development_journal_zh.md](D:/videotest/development_journal_zh.md)
  开发日志，记录遇到的问题、修复方法和关键 tradeoff。

- [pico4_ultra_device_bringup_zh.md](D:/videotest/pico4_ultra_device_bringup_zh.md)
  PICO 4 Ultra 真机联调与启动说明。

- `shared-protocol/`
  共享协议头文件和消息定义。

- `windows-native/`
  Windows 原生发送端、接收端和编码链路实现。

- `android-native/`
  Android Native + OpenXR + MediaCodec 头显端实现。

---

## 当前项目状态

当前已经完成或验证过的关键能力包括：

- PICO 4 Ultra 原生 OpenXR 应用可运行
- 头显姿态可通过 UDP 上行到 Windows
- Windows 原生内容可通过 NVENC 编码并通过 UDP 下行
- 头显端可用 MediaCodec 解码 H.264 并显示
- 姿态驱动的原生场景闭环已打通
- Windows 发送端已抽象为“共享发送核心 + 可插拔内容源”
- 已新增可切换显示模式：`quad_mono`、`projection_mono`、`projection_stereo`
- `projection_mono` 已验证可见，`projection_stereo` 已接通 true stereo projection 的最小代码骨架
- Android 端已支持：
  - 启动参数覆盖目标地址
  - 记住上次成功连接地址

---

## 当前沉浸式显示路线

当前项目不再只停留在“固定 quad 贴图”阶段，而是明确往 **OpenXR true stereo projection** 推进：

- `quad_mono`
  - 旧方案：单路视频作为 `XrCompositionLayerQuad` 提交，效果是“前方一块大屏”。
- `projection_mono`
  - 过渡方案：仍是单路解码，但改为走 `XrCompositionLayerProjection`，先验证 projection 显示链路与 compositor 行为。
- `projection_stereo`
  - 当前主线：Unity 左右眼分别输出 `RenderTexture`，Windows 原生插件分别编码发送，Android 按 `view_id` 路由到左右 decoder，再把左右眼 swapchain 提交给 OpenXR compositor。

当前推荐的联调脚本：

```powershell
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\start_unity_editor_projection_mono.ps1
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\start_unity_editor_projection_stereo.ps1
```

如果只是更换网络环境后重新同步目标地址与头显运行时配置，可执行：

```powershell
powershell -ExecutionPolicy Bypass -File D:\videotest\tools\sync_editor_runtime.ps1
```

---

## 中文文档入口

如果你希望直接阅读中文版本，请从这里开始：

- [documentation_zh_index.md](D:/videotest/documentation_zh_index.md)
- [immersive_video_display_plan_zh.md](D:/videotest/immersive_video_display_plan_zh.md)
- [immersive_projection_pathfinding_log_zh.md](D:/videotest/immersive_projection_pathfinding_log_zh.md)
- [startup_profiles_guide_zh.md](D:/videotest/startup_profiles_guide_zh.md)
