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
- Android 端已支持：
  - 启动参数覆盖目标地址
  - 记住上次成功连接地址

---

## 中文文档入口

如果你希望直接阅读中文版本，请从这里开始：

- [documentation_zh_index.md](D:/videotest/documentation_zh_index.md)
